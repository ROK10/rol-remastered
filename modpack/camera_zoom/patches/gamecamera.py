r"""camera_zoom: wider zoom range in data\gamecamera.xml (multiplayer_data.big).

Original REGULATION: 8 zoom levels, in 2000, out 7500 (extended 8600), zoom-out pan factor 450.
The pan factor grows with the distance (450 * 11000 / 7500 = 660) so scrolling at the far zoom keeps its speed.
"""
from entrypatch import set_attrs

REGULATION = {
    'regulation_num_zoom_levels': '12',
    'regulation_zoom_in_dist': '1500.',
    'regulation_zoom_out_dist': '11000.',
    'regulation_zoom_out_pan_factor': '660.',
    'regulation_extended_zoom_out_dist': '14000.',
}


def patch(text):
    return set_attrs(text, 'REGULATION', {}, REGULATION)
