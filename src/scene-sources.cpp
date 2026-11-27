#include "scene-sources.hpp"

#include "caption-feeds.hpp"

#include <obs-module.h>
#include <obs.hpp>
#include <graphics/graphics.h>
#include <plugin-support.h>

#include "third_party/qrcodegen/qrcodegen.h"

#include <QCoreApplication>
#include <QFont>
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
	bool showLink = true, showListeners = true, light = false;
};

static LinkOptions link_options(obs_data_t *s)
{
	LinkOptions o;
	o.headline = QString::fromUtf8(obs_data_get_string(s, "headline"));
	o.size = qBound(120, int(obs_data_get_int(s, "size")), 2000);
	o.showLink = obs_data_get_bool(s, "show_link");
	o.showListeners = obs_data_get_bool(s, "show_listeners");
	o.light = QString::fromUtf8(obs_data_get_string(s, "style")) == QLatin1String("light");
	return o;
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
	// The QR code of `text` drawn into `area`, dark modules on white with
	// its quiet zone; false when it cannot be encoded.
	static bool drawQr(QPainter &p, const QRectF &area, const QString &text)
	{
		std::vector<uint8_t> qr(qrcodegen_BUFFER_LEN_MAX), temp(qrcodegen_BUFFER_LEN_MAX);
		if (!qrcodegen_encodeText(text.toUtf8().constData(), temp.data(), qr.data(), qrcodegen_Ecc_MEDIUM,
					  qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
			return false;
		const int n = qrcodegen_getSize(qr.data());
		const int quiet = 2;
		const qreal module = qFloor(area.width() / (n + 2 * quiet));
		const qreal side = module * (n + 2 * quiet);
		const QRectF box(area.center().x() - side / 2, area.center().y() - side / 2, side, side);
		QPainterPath bg;
		bg.addRoundedRect(box, module * 1.5, module * 1.5);
		p.fillPath(bg, Qt::white);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(10, 12, 41));
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++)
				if (qrcodegen_getModule(qr.data(), x, y))
					p.drawRect(QRectF(box.left() + (x + quiet) * module, box.top() + (y + quiet) * module,
							  module, module));
		return true;
	}

	void paint()
	{
		SceneHub *hub = SceneHub::instance();
		const QString url = hub->broadcastUrl();
		const int s = opt.size;
		const int pad = qMax(8, s / 12);
		QFont head;
		head.setPixelSize(qMax(10, s / 11));
		head.setBold(true);
		QFont small;
		small.setPixelSize(qMax(9, s / 16));
		const QFontMetrics hm(head), sm(small);
		const int width = s + pad * 2;
		int height = pad + s + pad;
		if (!opt.headline.isEmpty())
			height += hm.lineSpacing() + pad / 2;
		if (opt.showLink)
			height += sm.lineSpacing() + pad / 3;
		if (opt.showListeners)
			height += sm.lineSpacing() + pad / 3;

		const QColor card = opt.light ? QColor(255, 255, 255) : QColor(10, 12, 41);
		const QColor fg = opt.light ? QColor(10, 12, 41) : QColor(255, 255, 255);
		const QColor muted = opt.light ? QColor(90, 96, 120) : QColor(190, 196, 220);

		QImage img(width, height, QImage::Format_ARGB32_Premultiplied);
		img.fill(Qt::transparent);
		QPainter p(&img);
		p.setRenderHint(QPainter::Antialiasing);
		p.setRenderHint(QPainter::TextAntialiasing);
		QPainterPath bg;
		bg.addRoundedRect(QRectF(0, 0, width, height), pad, pad);
		p.fillPath(bg, card);

		int y = pad;
		if (!opt.headline.isEmpty()) {
			p.setFont(head);
			p.setPen(fg);
			p.drawText(QRect(pad, y, s, hm.lineSpacing()), Qt::AlignCenter,
				   hm.elidedText(opt.headline, Qt::ElideRight, s));
			y += hm.lineSpacing() + pad / 2;
		}
		const QRectF qrArea(pad, y, s, s);
		if (url.isEmpty() || !drawQr(p, qrArea, url)) {
			QPainterPath empty;
			empty.addRoundedRect(qrArea, pad / 2.0, pad / 2.0);
			p.fillPath(empty, opt.light ? QColor(236, 238, 245) : QColor(30, 34, 70));
			p.setFont(small);
			p.setPen(muted);
			p.drawText(qrArea.adjusted(pad, pad, -pad, -pad), Qt::AlignCenter | Qt::TextWordWrap,
				   QString::fromUtf8(T("VocalStack.Source.NoBroadcast")));
		}
		y += s + pad / 2;
		p.setFont(small);
		if (opt.showLink) {
			const QString bare = QString(url).remove(QRegularExpression("^https?://"));
			p.setPen(fg);
			p.drawText(QRect(pad, y, s, sm.lineSpacing()), Qt::AlignCenter, sm.elidedText(bare, Qt::ElideMiddle, s));
			y += sm.lineSpacing() + pad / 3;
		}
		if (opt.showListeners) {
			const int n = hub->listeners();
			if (n >= 0 && !url.isEmpty()) {
				p.setPen(muted);
				const QString text = QString::fromUtf8(n == 1 ? T("VocalStack.Source.OneListening")
									      : T("VocalStack.Source.Listening"))
							     .arg(n);
				p.drawText(QRect(pad, y, s, sm.lineSpacing()), Qt::AlignCenter, text);
			}
		}
		p.end();
		publish(img);
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
	d->frame->cx = uint32_t(s + 2 * qMax(8, s / 12));
	d->frame->cy = uint32_t(s * 3 / 2);
	link_update(d, settings);
	return d;
}

static void link_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, "headline", T("VocalStack.Source.DefaultHeadline"));
	obs_data_set_default_int(s, "size", 360);
	obs_data_set_default_bool(s, "show_link", true);
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
	obs_properties_add_bool(props, "show_link", T("VocalStack.Source.ShowLink"));
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
