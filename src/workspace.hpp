#pragma once

#include <QJsonValue>
#include <QString>

// The workspace an API key belongs to: what the key makes lands there and
// is billed there (`workspace` of GET /v1/me and of the Connect token
// answer; an older API leaves it out).
struct Workspace {
	QString id, name;
	// The workspace the person got at sign-up.
	bool personal = false;
	// The workspace's own icon (a PNG), a link under the API's address that
	// needs no key; empty when it has none.
	QString iconUrl;

	// False when the API named no workspace.
	bool known() const { return !id.isEmpty(); }
	bool operator==(const Workspace &o) const
	{
		return id == o.id && name == o.name && personal == o.personal && iconUrl == o.iconUrl;
	}
	bool operator!=(const Workspace &o) const { return !(*this == o); }

	static Workspace fromJson(const QJsonValue &value);

	// Which icon the link shows: the link's path and its `v`, which stay
	// the same while the link is only signed again. Empty without an icon.
	QString iconVersion() const;
	// One or two letters for a workspace with no icon, as the dashboard
	// draws them.
	QString initials() const;
	// The tile's hue in degrees, the dashboard's choice for this workspace.
	int hue() const;
};
