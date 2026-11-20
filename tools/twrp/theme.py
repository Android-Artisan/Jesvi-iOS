"""Add JESViOS controls to the pinned TWRP portrait theme."""
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

path = Path(sys.argv[1])
source = path.read_text()
tree = ET.fromstring(source)
pages = tree.find("pages")
if pages is None:
    raise SystemExit("TWRP portrait theme has no pages")

if pages.find("page[@name='jesvios_wipe']") is None:
    wipe_button = """		<page name="wipe">
			<button style="main_button_half_height">
				<placement x="%indent%" y="%row14_y%"/>
				<text>JESViOS Wipe</text>
				<action function="page">jesvios_wipe</action>
			</button>"""
    mount_hint = """		<page name="mount">
			<text style="text_s">
				<placement x="%center_x%" y="%row17_y%" placement="5"/>
				<text>JESViOS volumes: raw images in Backup</text>
			</text>"""
    wipe_page = """		<page name="jesvios_wipe">
			<template name="page"/>
			<text style="text_l">
				<placement x="%col1_x_header%" y="%row3_header_y%"/>
				<text>JESViOS Wipe</text>
			</text>
			<text style="text_m">
				<placement x="%center_x%" y="%row3_y%" placement="5"/>
				<text>Fresh Install erases JESViOS Data.</text>
			</text>
			<text style="text_m">
				<placement x="%center_x%" y="%row4_y%" placement="5"/>
				<text>Reinstall replaces the JESViOS System.</text>
			</text>
			<text style="text_s">
				<placement x="%center_x%" y="%row6_y%" placement="5"/>
				<text>Use a JESViOS installer ZIP from SD or USB.</text>
			</text>
			<text style="text_s">
				<placement x="%center_x%" y="%row7_y%" placement="5"/>
				<text>The installer asks before writing.</text>
			</text>
			<button style="main_button_half_height">
				<placement x="%indent%" y="%row10_y%"/>
				<text>Choose installer ZIP</text>
				<actions>
					<action function="set">tw_selectimage=0</action>
					<action function="page">install</action>
				</actions>
			</button>
			<action>
				<touch key="back"/>
				<action function="page">wipe</action>
			</action>
			<action>
				<touch key="home"/>
				<action function="page">main</action>
			</action>
		</page>

"""
    for original, replacement in [
        ('\t\t<page name="wipe">', wipe_button),
        ('\t\t<page name="mount">', mount_hint),
        ('\t\t<page name="advancedwipe">', wipe_page + '\t\t<page name="advancedwipe">'),
    ]:
        if source.count(original) != 1:
            raise SystemExit(f"Unexpected TWRP theme page: {original}")
        source = source.replace(original, replacement, 1)
    ET.fromstring(source)
    path.write_text(source)
