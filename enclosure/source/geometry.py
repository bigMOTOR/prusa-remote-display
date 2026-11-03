# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""Hybrid CAD: fixed native STEP bases plus internal gussets and button pads.

Datums belong to the STEP bases, not independent design-option parameters.
The bases must be rebuilt when changing the assembly datum or functional fit.
"""
from pathlib import Path
import io
import math
import cadquery as cq
from OCP.gp import gp_Trsf
from OCP.STEPControl import STEPControl_Reader
from OCP.IFSelect import IFSelect_RetDone

ROOT = Path(__file__).resolve().parent
NAMES = ('shell', 'rear-cover', 'carrier', 'cassette')
S = math.sin(math.radians(6))
C = math.cos(math.radians(6))
BASE_Z = 31. - (8.9 - 4.) * S - .01 * C
BASE_Y = BASE_Z * math.tan(math.radians(6)) + 1.25 + .65 + (8.9 - 4.) / C
trsf = gp_Trsf()
trsf.SetValues(1, 0, 0, 0, 0, C, S, BASE_Y, 0, -S, C, BASE_Z)
T = cq.Matrix(trsf)
INV = cq.Matrix(trsf.Inverted())
GUSSET_OFFSET = (52., 74.06295683807632, 1.6756553217663656e-11)


def read(path):
    reader = STEPControl_Reader()
    assert reader.ReadStream(str(path), io.BytesIO(path.read_bytes())) == IFSelect_RetDone
    reader.TransferRoots()
    return cq.Shape.cast(reader.OneShape())


def box(x0, x1, y0, y1, z0, z1):
    return cq.Solid.makeBox(x1-x0, y1-y0, z1-z0, cq.Vector(x0, y0, z0))


def profile(points, y0, y1):
    wire = cq.Wire.makePolygon([cq.Vector(x, y0, z) for x, z in points], close=True)
    return cq.Solid.extrudeLinear(wire, [], cq.Vector(0, y1-y0, 0))


def fixed_envelopes():
    result = {}
    for side in (-1, 1):
        x = side * 38.45
        result.update({f'{side}:shaft': cq.Solid.makeCylinder(1.7, 18, cq.Vector(x, 10, 39.5), cq.Vector(0, 1, 0)),
                       f'{side}:nut': box(x-3.05, x+3.05, 18.30, 20.70, 36.55, 52.6),
                       f'{side}:bearing': box(x-3.05, x+3.05, 20.699, 20.701, 36.55, 42.45),
                       f'{side}:entry': box(x-3.35, x+3.35, 18.30, 40, 42.45, 51.5)})
    return result


def permanent_shell(original):
    # Order and exact Boolean operations preserve the accepted native topology.
    shell = original
    for side in (-1, 1):
        for family, points, yrange in (
            ('mid', [(13.35, 14.82), (16.65, 14.82), (16.65, 2.50), (15.40, 2.50), (15.40, 12.00)], (35.45, 36.75)),
            ('lower', [(13.35, 14.82), (14.10, 14.82), (14.10, 11.50), (13.90, 11.50)], (67.85, 68.70)),
        ):
            if side > 0:
                points = [(104-x, z) for x, z in points][::-1]
            addition = profile(points, *yrange).translate(tuple(-v for v in GUSSET_OFFSET)).rotate((0, 0, 0), (1, 0, 0), -96.)
            for envelope in fixed_envelopes().values():
                addition = addition.cut(envelope)
            shell = shell.fuse(addition)
    return shell


def build():
    working = {name: read(ROOT/'bases'/f'{name}.step') for name in (*NAMES, 'button')}
    working['shell'] = permanent_shell(working['shell'])
    groups = {name: {name: working[name]} for name in NAMES}
    groups['cassette']['button-functional'] = working['button']
    u = -22.160138
    pads = {
        'TEMP-remove-cap-pad': box(u-3.3, u+3.3, -1.8, -1.45, 31.1, 37.8),
        'TEMP-remove-lower-skirt-pad': box(u-3., u+3., -1.8, -1.15, 20.4, 26.4),
        'TEMP-remove-upper-skirt-pad': box(u-3., u+3., -1.8, -1.15, 30.2, 30.85),
    }
    for label, shape in pads.items():
        world = shape.transformShape(T)
        assert world.intersect(working['button']).Volume() < 1e-7
        assert world.intersect(working['cassette']).Volume() < 1e-7
        assert abs(world.distance(working['button'])-.25) < 1e-7
        groups['cassette'][label] = world
    return working, groups


def pose(name, shape):
    if name == 'shell':
        return shape.rotate((0, 0, 0), (1, 0, 0), 96.)
    if name == 'rear-cover':
        normal = max(shape.Faces(), key=lambda face: face.Area()).normalAt()
        return shape.rotate((0, 0, 0), (1, 0, 0), -90-math.degrees(math.atan2(normal.z, normal.y)))
    return shape.transformShape(INV).rotate((0, 0, 0), (1, 0, 0), -90 if name == 'carrier' else 90)


def bounds(shape):
    bound = shape.BoundingBox()
    return [getattr(bound, key) for key in ('xmin', 'xmax', 'ymin', 'ymax', 'zmin', 'zmax')]
