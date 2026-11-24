"""Yo-kai Watch 1 (Switch) game versions, keyed by the main program's build ID.

gen.py refuses a main NSO whose build ID is not listed here, so patch and hook addresses
(tools/patches.py) can never land on the wrong build. Add a version by dumping its exefs and
appending its build ID (first 32 hex digits are enough) with a label.
"""


class UnknownGameVersion(Exception):
    pass


# key -> (label, build id prefix)
VERSIONS = {
    'v1_3_0': ('1.3.0', 'b6e172353e696e656a538cebd5722661'),
    # 'v1_4_0': ('1.4.0', '<build id of the 1.4.0 update main>'),  # needed by the English translation mod
}


def select_build_id(build_id):
    for key, (label, bid) in VERSIONS.items():
        if build_id.lower().startswith(bid.lower()):
            return key
    known = ', '.join(f'{v[0]} ({v[1][:16]})' for v in VERSIONS.values())
    raise UnknownGameVersion(f'unknown main build ID {build_id[:16]}; supported: {known}')
