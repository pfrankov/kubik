"""Deterministic fifteen-color RGBA median cut for a 32x32 private glyph."""
from collections import Counter

PIXELS = 32 * 32


def feature(color):
    r, g, b, alpha = color
    return r * alpha // 255, g * alpha // 255, b * alpha // 255, alpha * 2


def channel(items):
    ranges = [max(item[2][axis] for item in items) - min(item[2][axis] for item in items)
              for axis in range(4)]
    axis = max(range(4), key=lambda value: ranges[value])
    return axis, ranges[axis] * sum(item[1] for item in items)


def split(items):
    axis, _ = channel(items)
    items = sorted(items, key=lambda item: (item[2][axis], item[0]))
    half, count = sum(item[1] for item in items) / 2, 0
    at = 1
    for at in range(1, len(items)):
        count += items[at - 1][1]
        if count >= half:
            break
    return items[:at], items[at:]


def centroid(items):
    population = sum(item[1] for item in items)
    alpha_sum = sum(item[0][3] * item[1] for item in items)
    rgb = tuple((sum(item[0][axis] * item[0][3] * item[1] for item in items) + alpha_sum // 2) // alpha_sum
                for axis in range(3))
    return (*rgb, (alpha_sum + population // 2) // population)


def initial_boxes(colors):
    histogram = Counter(color for color in colors if color[3])
    # Never average opaque interiors into semitransparent antialiased edges.
    groups = [[], []]
    for color, count in sorted(histogram.items()):
        groups[int(color[3] == 255)].append((color, count, feature(color)))
    return [items for items in groups if items]


def cut_boxes(boxes):
    while len(boxes) < 15:
        choices = [(channel(items)[1], index) for index, items in enumerate(boxes) if len(items) > 1]
        if not choices:
            break
        _, index = max(choices)
        left, right = split(boxes.pop(index))
        boxes.extend((left, right))
    return boxes


def palette_for(boxes):
    grouped = sorted(((centroid(items), items) for items in boxes), key=lambda item: item[0])
    palette, lookup = [(0, 0, 0, 0)], {}
    for color, items in grouped:
        if color not in palette:
            palette.append(color)
        index = palette.index(color)
        for item in items:
            lookup[item[0]] = index
    palette.extend([(0, 0, 0, 0)] * (16 - len(palette)))
    return palette, lookup


def pack_indices(colors, lookup):
    indices = [lookup[color] if color[3] else 0 for color in colors]
    return bytes((indices[index] << 4) | indices[index + 1] for index in range(0, PIXELS, 2))


def quantize(pixels):
    if not isinstance(pixels, bytes) or len(pixels) != PIXELS * 4:
        raise ValueError('Raw color glyph must be 32x32 straight RGBA')
    colors = [tuple(pixels[offset:offset + 4]) for offset in range(0, len(pixels), 4)]
    palette, lookup = palette_for(cut_boxes(initial_boxes(colors)))
    return bytes(channel for color in palette for channel in color) + pack_indices(colors, lookup)
