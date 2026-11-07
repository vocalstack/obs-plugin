.PHONY: notarized

# The newest Apple notarization of this repo's files and the version it was for.
notarized:
	@build-aux/notarized.py vocalstack-obs vocalstack/obs-plugin
