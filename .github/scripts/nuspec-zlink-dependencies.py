"""Prints the Zlink.* dependencies of a nuspec read from stdin as "<id>\t<version>".

A NuGet version range such as "[0.31.0, )" or "0.31.0" yields its lower bound, which is
the version the release publishes together with the package.
"""

import re
import sys
import xml.etree.ElementTree as ET

seen = set()
for item in ET.fromstring(sys.stdin.read()).iter():
    if item.tag.rsplit("}", 1)[-1] != "dependency":
        continue
    name = item.attrib.get("id", "")
    if not name.startswith("Zlink.") or name in seen:
        continue
    seen.add(name)
    version = re.sub(r"^[\[(]", "", item.attrib.get("version", "")).split(",")[0].strip(" )]")
    if version:
        print(f"{name}\t{version}")
