.PHONY: notarized

# The newest Apple notarization of this repo's files (keychain profile vocalstack-notary).
notarized:
	@xcrun notarytool history --keychain-profile vocalstack-notary --output-format json | python3 -c 'import json,sys; h=[x for x in json.load(sys.stdin)["history"] if x["name"].lower().startswith("vocalstack-obs")]; x=h[0] if h else None; print("  ".join([x["status"], x["name"], x["createdDate"], x["id"]]) if x else "no notarization yet")'
