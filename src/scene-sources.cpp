#include "scene-sources.hpp"

#include "caption-feeds.hpp"

#include <obs-module.h>
#include <obs.hpp>
#include <graphics/graphics.h>
#include <plugin-support.h>

#include "third_party/qrcodegen/qrcodegen.h"

#include <QCoreApplication>
#include <QFont>
#include <QHash>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRegularExpression>
#include <QTextDocument>
#include <QTextOption>
#include <QThread>
#include <QtMath>

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

// ---------------------------------------------------------------- the hub

SceneHub *SceneHub::instance()
{
	static SceneHub *hub = nullptr;
	if (!hub) {
		hub = new SceneHub;
		hub->moveToThread(QCoreApplication::instance()->thread());
	}
	return hub;
}

void SceneHub::setFeeds(CaptionFeeds *feeds)
{
	captionFeeds = feeds;
}

CaptionFeeds *SceneHub::feeds() const
{
	return static_cast<CaptionFeeds *>(captionFeeds.data());
}

void SceneHub::setBroadcast(const QString &u, bool isPrivate)
{
	if (u == url && isPrivate == priv)
		return;
	url = u;
	priv = isPrivate;
	emit broadcastChanged();
}

void SceneHub::setListeners(int count)
{
	if (count == listenersNow)
		return;
	listenersNow = count;
	emit listenersChanged();
}

// ---------------------------------------------------------------- drawing

// A source's picture, painted on the Qt main thread and uploaded to a
// texture on OBS's graphics thread.
struct Frame {
	std::mutex lock;
	QImage image; // Format_ARGB32: BGRA in memory, as GS_BGRA
	bool dirty = false;
	std::atomic<uint32_t> cx{0}, cy{0};
};

// Lives on the Qt main thread; paints the picture.
class Painter : public QObject {
	Q_OBJECT

public:
	explicit Painter(std::shared_ptr<Frame> f) : frame(std::move(f)) {}

protected:
	void publish(const QImage &image)
	{
		QImage img = image.convertToFormat(QImage::Format_ARGB32);
		frame->cx = uint32_t(img.width());
		frame->cy = uint32_t(img.height());
		std::lock_guard<std::mutex> hold(frame->lock);
		frame->image = std::move(img);
		frame->dirty = true;
	}

	std::shared_ptr<Frame> frame;
};

struct SourceData {
	obs_source_t *source = nullptr;
	std::shared_ptr<Frame> frame = std::make_shared<Frame>();
	QPointer<Painter> painter;
	// The graphics thread's own.
	gs_texture_t *tex = nullptr;
	uint32_t texCx = 0, texCy = 0;
};

static void run_on_main(QObject *target, std::function<void()> fn)
{
	if (!target)
		return;
	QMetaObject::invokeMethod(target, std::move(fn), Qt::QueuedConnection);
}

// A Painter made on whichever thread OBS creates the source on, moved to
// the Qt main thread where it paints.
template<class P> static P *make_painter(SourceData *d)
{
	auto *p = new P(d->frame);
	if (QThread::currentThread() != QCoreApplication::instance()->thread())
		p->moveToThread(QCoreApplication::instance()->thread());
	d->painter = p;
	return p;
}

static void destroy_source(void *data)
{
	auto *d = static_cast<SourceData *>(data);
	if (Painter *p = d->painter.data())
		p->deleteLater();
	if (d->tex) {
		obs_enter_graphics();
		gs_texture_destroy(d->tex);
		obs_leave_graphics();
	}
	delete d;
}

static uint32_t source_width(void *data)
{
	return static_cast<SourceData *>(data)->frame->cx;
}

static uint32_t source_height(void *data)
{
	return static_cast<SourceData *>(data)->frame->cy;
}

static void render_source(void *data, gs_effect_t *)
{
	auto *d = static_cast<SourceData *>(data);
	{
		std::lock_guard<std::mutex> hold(d->frame->lock);
		if (d->frame->dirty) {
			d->frame->dirty = false;
			const QImage &img = d->frame->image;
			const uint32_t w = uint32_t(img.width()), h = uint32_t(img.height());
			if (d->tex && (w != d->texCx || h != d->texCy)) {
				gs_texture_destroy(d->tex);
				d->tex = nullptr;
			}
			if (w && h) {
				const uint8_t *bits = img.constBits();
				if (d->tex)
					gs_texture_set_image(d->tex, bits, uint32_t(img.bytesPerLine()), false);
				else
					d->tex = gs_texture_create(w, h, GS_BGRA, 1, &bits, GS_DYNAMIC);
				d->texCx = w;
				d->texCy = h;
			}
		}
	}
	if (!d->tex)
		return;
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_technique_t *tech = gs_effect_get_technique(effect, "Draw");
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), d->tex);
	gs_draw_sprite(d->tex, 0, d->texCx, d->texCy);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
	gs_enable_framebuffer_srgb(previous);
}

// OBS keeps colours as 0xAABBGGRR.
static QColor obs_color(long long c)
{
	return QColor(int(c & 0xff), int((c >> 8) & 0xff), int((c >> 16) & 0xff), int((c >> 24) & 0xff));
}

static const char *T(const char *key)
{
	return obs_module_text(key);
}

// ---------------------------------------------------------------- captions

struct CaptionOptions {
	QString lang;
	QString family = QStringLiteral("Arial");
	int size = 48;
	bool bold = true, italic = false;
	int width = 1280;
	int lines = 2;
	QColor text = Qt::white, background = QColor(10, 12, 41, 190);
	bool speakers = true;
	bool left = false;
};

static CaptionOptions caption_options(obs_data_t *s)
{
	CaptionOptions o;
	o.lang = QString::fromUtf8(obs_data_get_string(s, "lang"));
	OBSDataAutoRelease font = obs_data_get_obj(s, "font");
	if (font) {
		const QString face = QString::fromUtf8(obs_data_get_string(font, "face"));
		if (!face.isEmpty())
			o.family = face;
		o.size = qBound(12, int(obs_data_get_int(font, "size")), 300);
		const long long flags = obs_data_get_int(font, "flags");
		o.bold = flags & OBS_FONT_BOLD;
		o.italic = flags & OBS_FONT_ITALIC;
	}
	o.width = qBound(200, int(obs_data_get_int(s, "width")), 7680);
	o.lines = qBound(1, int(obs_data_get_int(s, "lines")), 6);
	o.text = obs_color(obs_data_get_int(s, "color") | 0xFF000000LL); // no alpha of its own
	o.background = obs_color(obs_data_get_int(s, "background"));
	o.speakers = obs_data_get_bool(s, "speakers");
	o.left = QString::fromUtf8(obs_data_get_string(s, "align")) == QLatin1String("left");
	return o;
}

class CaptionPainter : public Painter {
	Q_OBJECT

public:
	using Painter::Painter;
	~CaptionPainter() override
	{
		if (CaptionFeeds *feeds = SceneHub::instance()->feeds(); feeds && subscribed)
			feeds->release(opt.lang);
	}

	void apply(const CaptionOptions &o)
	{
		CaptionFeeds *feeds = SceneHub::instance()->feeds();
		if (feeds && (!subscribed || o.lang != opt.lang)) {
			if (subscribed)
				feeds->release(opt.lang);
			feeds->acquire(o.lang);
			subscribed = true;
			speaker.clear();
			text.clear();
			if (!connected) {
				connect(feeds, &CaptionFeeds::caption, this,
					[this](const QString &lang, const QString &who, const QString &words) {
						if (lang != opt.lang)
							return;
						speaker = who;
						text = words;
						paint();
					});
				connected = true;
			}
		}
		opt = o;
		paint();
	}

private:
	QString html(const QStringList &words, bool cut) const
	{
		QString h;
		if (opt.speakers && !speaker.isEmpty())
			h = QStringLiteral("<span style='color:#55abe5; font-weight:600;'>%1:</span> ").arg(speaker.toHtmlEscaped());
		return h + (cut ? QStringLiteral("…") : QString()) + words.join(' ').toHtmlEscaped();
	}

	// The canvas is always as big as `lines` lines, so the source keeps its
	// place in the scene; the box behind the words is as big as they are,
	// at the bottom.
	void paint()
	{
		QFont font(opt.family);
		font.setPixelSize(opt.size);
		font.setBold(opt.bold);
		font.setItalic(opt.italic);
		const QFontMetrics fm(font);
		const int pad = qMax(4, opt.size / 3);
		const int height = opt.lines * fm.lineSpacing() + pad * 2;
		QImage img(opt.width, height, QImage::Format_ARGB32_Premultiplied);
		img.fill(Qt::transparent);
		if (!text.isEmpty()) {
			QTextDocument doc;
			doc.setDefaultFont(font);
			doc.setDocumentMargin(0);
			doc.setDefaultStyleSheet(QStringLiteral("body { color: %1; }").arg(opt.text.name()));
			QTextOption to(opt.left ? Qt::AlignLeft : Qt::AlignHCenter);
			to.setWrapMode(QTextOption::WordWrap);
			doc.setDefaultTextOption(to);
			const int maxWidth = opt.width - pad * 2;
			doc.setTextWidth(maxWidth);
			// Older words go first until the lines hold the rest.
			QStringList words = text.split(' ', Qt::SkipEmptyParts);
			bool cut = false;
			doc.setHtml(html(words, false));
			while (doc.size().height() > (opt.lines + 0.2) * fm.lineSpacing() && words.size() > 1) {
				words.removeFirst();
				cut = true;
				doc.setHtml(html(words, cut));
			}
			doc.setTextWidth(qMin<qreal>(maxWidth, doc.idealWidth() + 1));
			const QSizeF size = doc.size();
			QRectF box(0, 0, size.width() + pad * 2, size.height() + pad);
			box.moveBottom(height);
			if (opt.left)
				box.moveLeft(0);
			else
				box.moveLeft((opt.width - box.width()) / 2.0);
			QPainter p(&img);
			p.setRenderHint(QPainter::Antialiasing);
			p.setRenderHint(QPainter::TextAntialiasing);
			if (opt.background.alpha() > 0) {
				QPainterPath path;
				path.addRoundedRect(box, pad * 0.6, pad * 0.6);
				p.fillPath(path, opt.background);
			}
			p.translate(box.left() + pad, box.top() + pad / 2.0);
			doc.drawContents(&p);
		}
		publish(img);
	}

	CaptionOptions opt;
	QString speaker, text;
	bool subscribed = false, connected = false;
};

static void caption_update(void *data, obs_data_t *settings)
{
	auto *d = static_cast<SourceData *>(data);
	const CaptionOptions o = caption_options(settings);
	QPointer<Painter> p = d->painter;
	run_on_main(p, [p, o]() {
		if (auto *cp = qobject_cast<CaptionPainter *>(p.data()))
			cp->apply(o);
	});
}

static void *caption_create(obs_data_t *settings, obs_source_t *source)
{
	auto *d = new SourceData;
	d->source = source;
	make_painter<CaptionPainter>(d);
	// A size at once, before the first paint: the scene places it.
	d->frame->cx = uint32_t(qBound(200, int(obs_data_get_int(settings, "width")), 7680));
	d->frame->cy = 160;
	caption_update(d, settings);
	return d;
}

static void caption_defaults(obs_data_t *s)
{
	OBSDataAutoRelease font = obs_data_create();
#ifdef _WIN32
	obs_data_set_default_string(font, "face", "Arial");
#elif defined(__APPLE__)
	obs_data_set_default_string(font, "face", "Helvetica Neue");
#else
	obs_data_set_default_string(font, "face", "Sans Serif");
#endif
	obs_data_set_default_int(font, "size", 48);
	obs_data_set_default_int(font, "flags", OBS_FONT_BOLD);
	obs_data_set_default_obj(s, "font", font);
	obs_data_set_default_string(s, "lang", "");
	obs_data_set_default_int(s, "width", 1280);
	obs_data_set_default_int(s, "lines", 2);
	obs_data_set_default_int(s, "color", 0xFFFFFFFF);
	obs_data_set_default_int(s, "background", 0xBE290C0A); // VocalStack navy at 75 %
	obs_data_set_default_bool(s, "speakers", true);
	obs_data_set_default_string(s, "align", "center");
}

static obs_properties_t *caption_properties(void *data)
{
	auto *d = static_cast<SourceData *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *lang = obs_properties_add_list(props, "lang", T("VocalStack.Source.Language"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(lang, T("VocalStack.Source.AsSpoken"), "");
	const auto langs = SceneHub::instance()->translationLanguages();
	for (const auto &[name, code] : langs)
		obs_property_list_add_string(lang, name.toUtf8().constData(), code.toUtf8().constData());
	if (d && d->source) {
		// The saved language stays choosable while the list is not loaded.
		OBSDataAutoRelease s = obs_source_get_settings(d->source);
		const char *cur = obs_data_get_string(s, "lang");
		bool found = !*cur;
		for (const auto &l : langs)
			found = found || l.second == QString::fromUtf8(cur);
		if (!found)
			obs_property_list_add_string(lang, cur, cur);
	}
	obs_property_set_long_description(lang, T("VocalStack.Source.LanguageNote"));
	obs_properties_add_font(props, "font", T("VocalStack.Source.Font"));
	obs_properties_add_color(props, "color", T("VocalStack.Source.TextColor"));
	obs_properties_add_color_alpha(props, "background", T("VocalStack.Source.Background"));
	obs_properties_add_int(props, "width", T("VocalStack.Source.Width"), 200, 7680, 10);
	obs_properties_add_int_slider(props, "lines", T("VocalStack.Source.Lines"), 1, 6, 1);
	obs_property_t *align = obs_properties_add_list(props, "align", T("VocalStack.Source.Align"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(align, T("VocalStack.Source.Center"), "center");
	obs_property_list_add_string(align, T("VocalStack.Source.Left"), "left");
	obs_properties_add_bool(props, "speakers", T("VocalStack.Source.Speakers"));
	obs_properties_add_text(props, "note", T("VocalStack.Source.CaptionsNote"), OBS_TEXT_INFO);
	return props;
}

static const char *caption_name(void *)
{
	return T("VocalStack.CaptionsSource");
}

// ---------------------------------------------------------------- the link

struct LinkOptions {
	QString headline;
	int size = 360;
	bool showListeners = true, light = false;
};

static LinkOptions link_options(obs_data_t *s)
{
	LinkOptions o;
	o.headline = QString::fromUtf8(obs_data_get_string(s, "headline"));
	o.size = qBound(120, int(obs_data_get_int(s, "size")), 2000);
	o.showListeners = obs_data_get_bool(s, "show_listeners");
	o.light = QString::fromUtf8(obs_data_get_string(s, "style")) == QLatin1String("light");
	return o;
}

// The VocalStack logo shipped with the plugin: white for a dark card, dark
// for a light one. Read once.
static QImage logoImage(bool white)
{
	static QImage cache[2];
	QImage &img = cache[white ? 1 : 0];
	if (img.isNull()) {
		char *path = obs_module_file(white ? "logo-white@2x.png" : "logo@2x.png");
		if (path)
			img.load(QString::fromUtf8(path));
		bfree(path);
	}
	return img;
}

class LinkPainter : public Painter {
	Q_OBJECT

public:
	explicit LinkPainter(std::shared_ptr<Frame> f) : Painter(std::move(f))
	{
		connect(SceneHub::instance(), &SceneHub::broadcastChanged, this, &LinkPainter::paint);
		connect(SceneHub::instance(), &SceneHub::listenersChanged, this, &LinkPainter::paint);
	}

	void apply(const LinkOptions &o)
	{
		opt = o;
		paint();
	}

private:
	// The QR code of `text` on a rounded white plate, centred in `area`:
	// whole-pixel modules drawn without anti-aliasing, so no seams show
	// between them; false when it cannot be encoded.
	static bool drawQr(QPainter &p, const QRect &area, const QString &text, const QColor &ink)
	{
		std::vector<uint8_t> qr(qrcodegen_BUFFER_LEN_MAX), temp(qrcodegen_BUFFER_LEN_MAX);
		if (!qrcodegen_encodeText(text.toUtf8().constData(), temp.data(), qr.data(), qrcodegen_Ecc_MEDIUM,
					  qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
			return false;
		const int n = qrcodegen_getSize(qr.data());
		const int quiet = 2;
		const int module = qMax(1, area.width() / (n + 2 * quiet));
		const int side = module * (n + 2 * quiet);
		const QRect box(area.left() + (area.width() - side) / 2, area.top() + (area.height() - side) / 2, side, side);
		QPainterPath plate;
		plate.addRoundedRect(QRectF(box), module * 1.5, module * 1.5);
		p.fillPath(plate, Qt::white);
		QPainterPath modules;
		for (int y = 0; y < n; y++) {
			// Runs of dark modules in a row as one rectangle each.
			for (int x = 0; x < n;) {
				if (!qrcodegen_getModule(qr.data(), x, y)) {
					x++;
					continue;
				}
				int end = x;
				while (end < n && qrcodegen_getModule(qr.data(), end, y))
					end++;
				modules.addRect(box.left() + (x + quiet) * module, box.top() + (y + quiet) * module,
						(end - x) * module, module);
				x = end;
			}
		}
		p.save();
		p.setRenderHint(QPainter::Antialiasing, false);
		p.fillPath(modules, ink);
		p.restore();
		return true;
	}

	// The QR code next to a phone showing what it opens: a language picked,
	// live captions in it, and the translation to listen to. Above them the
	// VocalStack logo (and, while live, how many are listening) and the
	// headline. The link itself is not written out: its length is the
	// person's, and the code carries it.
	void paint()
	{
		SceneHub *hub = SceneHub::instance();
		const QString url = hub->broadcastUrl();
		const qreal S = opt.size;
		const bool light = opt.light;

		const QColor card = light ? QColor(0xFF, 0xFF, 0xFF) : QColor(0x0A, 0x0C, 0x29);
		const QColor fg = light ? QColor(0x0A, 0x0C, 0x29) : QColor(0xFF, 0xFF, 0xFF);
		const QColor muted = light ? QColor(0x5A, 0x60, 0x78) : QColor(0xBE, 0xC4, 0xDC);
		const QColor panel = light ? QColor(0xEE, 0xF0, 0xF6) : QColor(0x1B, 0x1F, 0x4A);
		const QColor line = light ? QColor(0xD5, 0xD9, 0xE6) : QColor(0x34, 0x3A, 0x75);
		const QColor coral = light ? QColor(0xE0, 0x53, 0x3C) : QColor(0xEC, 0x63, 0x4D);
		const QColor screen(0xF4, 0xF5, 0xFA);
		const QColor ink(0x0A, 0x0C, 0x29);

		const qreal pad = S / 12, gap = 0.075 * S, phoneW = 0.5 * S, logoH = S / 9;
		const qreal W = 2 * pad + S + gap + phoneW;

		auto font = [](qreal px, QFont::Weight weight) {
			QFont f;
			f.setPixelSize(qMax(1, qRound(px)));
			f.setWeight(weight);
			return f;
		};
		const QFont headFont = font(qMax<qreal>(10, 0.08 * S), QFont::Bold);
		const QFontMetricsF hm(headFont);
		// The headline wraps to two lines at most; the card never widens.
		const qreal headW = W - 2 * pad;
		const bool twoLines = !opt.headline.isEmpty() && hm.horizontalAdvance(opt.headline) > headW;
		qreal bodyY = opt.headline.isEmpty() ? pad + logoH + 0.07 * S : pad + logoH + 0.22 * S;
		if (twoLines)
			bodyY += hm.lineSpacing();
		const qreal H = bodyY + S + pad;

		QImage img(qCeil(W), qCeil(H), QImage::Format_ARGB32_Premultiplied);
		img.fill(Qt::transparent);
		QPainter p(&img);
		p.setRenderHint(QPainter::Antialiasing);
		p.setRenderHint(QPainter::TextAntialiasing);
		p.setRenderHint(QPainter::SmoothPixmapTransform);

		// 1. The card.
		QPainterPath cardPath;
		cardPath.addRoundedRect(QRectF(0, 0, W, H), pad, pad);
		p.fillPath(cardPath, card);

		// 2. The logo, and how many are listening.
		const QImage logo = logoImage(!light);
		if (!logo.isNull())
			p.drawImage(QRectF(pad, pad, logoH * logo.width() / logo.height(), logoH), logo);
		const int n = hub->listeners();
		if (opt.showListeners && n >= 0 && !url.isEmpty()) {
			const QString text = QString::fromUtf8(n == 1 ? T("VocalStack.Source.OneListening")
								      : T("VocalStack.Source.Listening"))
						     .arg(n);
			const QFont countFont = font(qMax<qreal>(9, 0.045 * S), QFont::DemiBold);
			const qreal h = 0.085 * S;
			const qreal w = 0.0425 * S + 0.056 * S + QFontMetricsF(countFont).horizontalAdvance(text) + 0.038 * S;
			const QRectF pill(W - pad - w, pad + logoH / 2 - h / 2, w, h);
			QPainterPath pp;
			pp.addRoundedRect(pill, h / 2, h / 2);
			p.fillPath(pp, panel);
			const QPointF dot(pill.left() + 0.0425 * S, pill.center().y());
			p.setPen(Qt::NoPen);
			QColor halo = coral;
			halo.setAlpha(0x47);
			p.setBrush(halo);
			p.drawEllipse(dot, 0.026 * S, 0.026 * S);
			p.setBrush(coral);
			p.drawEllipse(dot, 0.013 * S, 0.013 * S);
			p.setFont(countFont);
			p.setPen(fg);
			p.drawText(QRectF(dot.x() + 0.056 * S, pill.top(), w, h), Qt::AlignLeft | Qt::AlignVCenter, text);
		}

		// 3. The headline.
		if (!opt.headline.isEmpty()) {
			p.setFont(headFont);
			p.setPen(fg);
			const QRectF r(pad, pad + logoH + 0.04 * S, headW, 0.14 * S + (twoLines ? hm.lineSpacing() : 0));
			if (!twoLines) {
				p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, opt.headline);
			} else {
				// Two lines at most, the end of the second elided.
				QStringList words = opt.headline.split(' ', Qt::SkipEmptyParts);
				QString first;
				while (!words.isEmpty()) {
					const QString next = first.isEmpty() ? words.first() : first + ' ' + words.first();
					if (!first.isEmpty() && hm.horizontalAdvance(next) > headW)
						break;
					first = next;
					words.removeFirst();
				}
				const QString second = hm.elidedText(words.join(' '), Qt::ElideRight, headW);
				const qreal top = r.center().y() - hm.lineSpacing();
				p.drawText(QRectF(pad, top, headW, hm.lineSpacing()), Qt::AlignLeft | Qt::AlignVCenter,
					   hm.elidedText(first, Qt::ElideRight, headW));
				p.drawText(QRectF(pad, top + hm.lineSpacing(), headW, hm.lineSpacing()),
					   Qt::AlignLeft | Qt::AlignVCenter, second);
			}
		}

		// 4. The code, or where it will be.
		const QRect qrArea(qRound(pad), qRound(bodyY), qRound(S), qRound(S));
		if (!url.isEmpty() && drawQr(p, qrArea, url, ink)) {
			if (light) {
				const qreal w = qMax<qreal>(1.0, 0.0055 * S);
				const int module = qMax(1, qrArea.width() / (qrcodegenSize(url) + 4));
				const int side = module * (qrcodegenSize(url) + 4);
				const QRectF plate(qrArea.left() + (qrArea.width() - side) / 2, qrArea.top() + (qrArea.height() - side) / 2,
						   side, side);
				p.setPen(QPen(line, w));
				p.setBrush(Qt::NoBrush);
				p.drawRoundedRect(plate.adjusted(w / 2, w / 2, -w / 2, -w / 2), module * 1.5, module * 1.5);
			}
		} else {
			const qreal w = 0.006 * S;
			const QRectF slot = QRectF(qrArea).adjusted(w / 2, w / 2, -w / 2, -w / 2);
			QPen dash(line, w);
			dash.setDashPattern({5.0, 3.33});
			p.setPen(dash);
			p.setBrush(panel);
			p.drawRoundedRect(slot, 0.04 * S, 0.04 * S);
			p.setFont(font(qMax<qreal>(9, 0.05 * S), QFont::Medium));
			p.setPen(muted);
			p.drawText(QRectF(qrArea).adjusted(0.1 * S, 0.1 * S, -0.1 * S, -0.1 * S), Qt::AlignCenter | Qt::TextWordWrap,
				   QString::fromUtf8(T("VocalStack.Source.NoBroadcast")));
		}

		// 5. The chevron from the code to the phone.
		{
			const qreal cx = pad + S + gap / 2, cy = bodyY + S / 2;
			p.setPen(QPen(coral, 0.012 * S, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			p.setBrush(Qt::NoBrush);
			const QPointF pts[3] = {{cx - 0.011 * S, cy - 0.022 * S}, {cx + 0.011 * S, cy}, {cx - 0.011 * S, cy + 0.022 * S}};
			p.drawPolyline(pts, 3);
		}

		// 6. The phone.
		const qreal px = pad + S + gap, py = bodyY;
		{
			const qreal sw = 0.008 * S;
			p.setPen(QPen(line, sw));
			p.setBrush(panel);
			p.drawRoundedRect(QRectF(px, py, phoneW, S).adjusted(sw / 2, sw / 2, -sw / 2, -sw / 2), 0.075 * S, 0.075 * S);
		}
		const qreal sx = px + 0.022 * S, sy = py + 0.022 * S, sW = 0.456 * S, sH = 0.956 * S;
		p.setPen(Qt::NoPen);
		p.setBrush(screen);
		p.drawRoundedRect(QRectF(sx, sy, sW, sH), 0.055 * S, 0.055 * S);
		p.setBrush(ink);
		p.drawRoundedRect(QRectF(px + 0.19 * S, sy + 0.022 * S, 0.12 * S, 0.026 * S), 0.013 * S, 0.013 * S);

		// What the phone shows: a language other than OBS's own.
		const bool french = QString::fromUtf8(obs_get_locale()).startsWith(QLatin1String("es"));
		const QString langName = french ? QStringLiteral("Français") : QStringLiteral("Español");
		const QString earlier = french ? QStringLiteral("Bienvenue !") : QStringLiteral("¡Bienvenidos!");
		const QStringList live = french ? QStringList{QStringLiteral("Aujourd'hui je"), QStringLiteral("vous montre la"),
							       QStringLiteral("scène dans OBS")}
						: QStringList{QStringLiteral("Hoy os enseño"), QStringLiteral("cómo montar la"),
							      QStringLiteral("escena en OBS")};

		const qreal m = 0.04 * S;
		const qreal lx = sx + m, ly = sy + 0.085 * S;
		p.setBrush(coral);
		p.drawRoundedRect(QRectF(lx, ly, 0.27 * S, 0.075 * S), 0.0375 * S, 0.0375 * S);
		p.setFont(font(0.043 * S, QFont::Bold));
		p.setPen(Qt::white);
		p.drawText(QRectF(lx + 0.035 * S, ly, 0.27 * S, 0.075 * S), Qt::AlignLeft | Qt::AlignVCenter, langName);
		{
			const qreal cx = lx + 0.228 * S, cy = ly + 0.0375 * S;
			p.setPen(QPen(Qt::white, 0.008 * S, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			const QPointF pts[3] = {{cx - 0.012 * S, cy - 0.006 * S}, {cx, cy + 0.006 * S}, {cx + 0.012 * S, cy - 0.006 * S}};
			p.drawPolyline(pts, 3);
		}

		const qreal ah = 0.105 * S, ay = sy + sH - 0.03 * S - ah;
		const qreal T0 = (ly + 0.075 * S + ay) / 2 - 0.1375 * S;
		const qreal rowW = sW - 2 * m, rowH = 0.065 * S;
		QColor faded = ink;
		faded.setAlpha(0x61);
		p.setFont(font(0.05 * S, QFont::DemiBold));
		p.setPen(faded);
		p.drawText(QRectF(sx + m, T0, rowW, rowH), Qt::AlignLeft | Qt::AlignVCenter, earlier);
		p.setFont(font(0.05 * S, QFont::Bold));
		p.setPen(ink);
		for (int i = 0; i < live.size(); i++)
			p.drawText(QRectF(sx + m, T0 + 0.080 * S + i * 0.065 * S, rowW, rowH), Qt::AlignLeft | Qt::AlignVCenter, live[i]);
		p.setPen(Qt::NoPen);
		p.setBrush(coral);
		p.drawRoundedRect(QRectF(sx + m, T0 + 0.275 * S, 0.09 * S, 0.012 * S), 0.006 * S, 0.006 * S);

		// The audio bar: play, and a waveform like the bars in the logo's V.
		p.setBrush(ink);
		p.drawRoundedRect(QRectF(sx + 0.024 * S, ay, sW - 0.048 * S, ah), ah / 2, ah / 2);
		const QPointF play(sx + 0.024 * S + ah / 2, ay + ah / 2);
		p.setBrush(coral);
		p.drawEllipse(play, 0.0378 * S, 0.0378 * S);
		const qreal tr = 0.0189 * S;
		const QPointF tri[3] = {{play.x() - 0.6 * tr, play.y() - tr}, {play.x() + tr, play.y()}, {play.x() - 0.6 * tr, play.y() + tr}};
		p.setBrush(Qt::white);
		p.drawPolygon(tri, 3);
		static const qreal bars[] = {0.025, 0.045, 0.060, 0.035, 0.050, 0.030, 0.042, 0.022};
		const qreal bw = 0.014 * S;
		for (int i = 0; i < 8; i++) {
			const qreal h = bars[i] * S;
			p.drawRoundedRect(QRectF(play.x() + 0.0378 * S + 0.03 * S + i * 0.026 * S, play.y() - h / 2, bw, h), bw / 2,
					  bw / 2);
		}

		p.end();
		publish(img);
	}

	// The QR code's size in modules for `text`, as drawQr encodes it; 0
	// when it cannot be encoded.
	static int qrcodegenSize(const QString &text)
	{
		std::vector<uint8_t> qr(qrcodegen_BUFFER_LEN_MAX), temp(qrcodegen_BUFFER_LEN_MAX);
		if (!qrcodegen_encodeText(text.toUtf8().constData(), temp.data(), qr.data(), qrcodegen_Ecc_MEDIUM,
					  qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
			return 0;
		return qrcodegen_getSize(qr.data());
	}

	LinkOptions opt;
};

static void link_update(void *data, obs_data_t *settings)
{
	auto *d = static_cast<SourceData *>(data);
	const LinkOptions o = link_options(settings);
	QPointer<Painter> p = d->painter;
	run_on_main(p, [p, o]() {
		if (auto *lp = qobject_cast<LinkPainter *>(p.data()))
			lp->apply(o);
	});
}

static void *link_create(obs_data_t *settings, obs_source_t *source)
{
	auto *d = new SourceData;
	d->source = source;
	make_painter<LinkPainter>(d);
	const int s = qBound(120, int(obs_data_get_int(settings, "size")), 2000);
	d->frame->cx = uint32_t(s * 1.75);
	d->frame->cy = uint32_t(s * 1.5);
	link_update(d, settings);
	return d;
}

static void link_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, "headline", T("VocalStack.Source.DefaultHeadline"));
	obs_data_set_default_int(s, "size", 360);
	obs_data_set_default_bool(s, "show_listeners", true);
	obs_data_set_default_string(s, "style", "dark");
}

static obs_properties_t *link_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "headline", T("VocalStack.Source.Headline"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "size", T("VocalStack.Source.QrSize"), 120, 2000, 10);
	obs_property_t *style = obs_properties_add_list(props, "style", T("VocalStack.Source.Style"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(style, T("VocalStack.Source.Dark"), "dark");
	obs_property_list_add_string(style, T("VocalStack.Source.Light"), "light");
	obs_properties_add_bool(props, "show_listeners", T("VocalStack.Source.ShowListeners"));
	obs_properties_add_text(props, "note", T("VocalStack.Source.LinkNote"), OBS_TEXT_INFO);
	return props;
}

static const char *link_name(void *)
{
	return T("VocalStack.LinkSource");
}

// ---------------------------------------------------------------- register

void register_scene_sources()
{
	SceneHub::instance();

	obs_source_info captions = {};
	captions.id = "vocalstack_captions";
	captions.type = OBS_SOURCE_TYPE_INPUT;
	captions.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	captions.icon_type = OBS_ICON_TYPE_TEXT;
	captions.get_name = caption_name;
	captions.create = caption_create;
	captions.destroy = destroy_source;
	captions.update = caption_update;
	captions.get_defaults = caption_defaults;
	captions.get_properties = caption_properties;
	captions.get_width = source_width;
	captions.get_height = source_height;
	captions.video_render = render_source;
	obs_register_source(&captions);

	obs_source_info link = {};
	link.id = "vocalstack_broadcast_link";
	link.type = OBS_SOURCE_TYPE_INPUT;
	link.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	link.icon_type = OBS_ICON_TYPE_IMAGE;
	link.get_name = link_name;
	link.create = link_create;
	link.destroy = destroy_source;
	link.update = link_update;
	link.get_defaults = link_defaults;
	link.get_properties = link_properties;
	link.get_width = source_width;
	link.get_height = source_height;
	link.video_render = render_source;
	obs_register_source(&link);
}

#include "scene-sources.moc"
