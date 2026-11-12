#include "workspace.hpp"

#include <QChar>
#include <QJsonObject>
#include <QList>
#include <QUrl>
#include <QUrlQuery>

Workspace Workspace::fromJson(const QJsonValue &value)
{
	Workspace w;
	const QJsonObject o = value.toObject();
	w.id = o.value("id").toString();
	if (w.id.isEmpty())
		return w;
	w.name = o.value("name").toString();
	w.personal = o.value("personal").toBool();
	w.iconUrl = o.value("iconUrl").toString();
	return w;
}

QString Workspace::iconVersion() const
{
	if (iconUrl.isEmpty())
		return QString();
	const QUrl url(iconUrl);
	return url.path() + "?v=" + QUrlQuery(url).queryItemValue("v");
}

QString Workspace::initials() const
{
	// Each word's letters and digits, for the words that have any.
	QList<QList<uint>> words;
	QList<uint> word;
	bool inWord = false;
	const QList<uint> points = (name + QChar(' ')).toUcs4();
	for (const uint c : points) {
		if (!QChar::isSpace(c)) {
			inWord = true;
			if (QChar::isLetterOrNumber(c))
				word.append(c);
			continue;
		}
		if (inWord && !word.isEmpty())
			words.append(word);
		word.clear();
		inWord = false;
	}
	if (words.isEmpty())
		return QStringLiteral("?");
	QList<uint> picked;
	if (words.size() == 1)
		picked = words.at(0).mid(0, 2);
	else
		picked = {words.at(0).at(0), words.at(1).at(0)};
	QString s;
	for (const uint c : picked) {
		if (QChar::requiresSurrogates(c)) {
			s += QChar(QChar::highSurrogate(c));
			s += QChar(QChar::lowSurrogate(c));
		} else {
			s += QChar(char16_t(c));
		}
	}
	return s.toUpper();
}

int Workspace::hue() const
{
	static const int HUES[] = {252, 200, 160, 128, 85, 40, 12, 330, 296};
	// 32-bit FNV-1a over the id's UTF-16 units.
	quint32 h = 2166136261u;
	for (const QChar c : id) {
		h ^= c.unicode();
		h *= 16777619u;
	}
	return HUES[h % 9];
}
