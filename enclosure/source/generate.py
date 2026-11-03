# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""Regenerate assembly STEP, print-pose STL and a geometry-only 3MF.

Requires CadQuery 2.6.1 and PrusaSlicer 2.9.6 for the frozen export behaviour.
Does not slice, emit G-code, connect to a printer, or modify input bases.
"""
from pathlib import Path
import argparse
import json
import re
import shutil
import subprocess
import zipfile
import xml.etree.ElementTree as ET
import cadquery as cq
from geometry import ROOT, NAMES, bounds, build, pose

NS = 'http://schemas.microsoft.com/3dmanufacturing/core/2015/02'
N = {'m': NS}
ET.register_namespace('', NS)


def tag(name):
    return '{'+NS+'}'+name


def clean_step(path):
    text = path.read_text()
    text = re.sub(r"FILE_NAME\('[^']*'", "FILE_NAME('"+path.name+"'", text)
    path.write_text(text)


def pack_indexed(source, target):
    with zipfile.ZipFile(source) as archive:
        root = ET.fromstring(archive.read('3D/3dmodel.model'))
        config = ET.fromstring(archive.read('Metadata/Slic3r_PE_model.config'))
        for obj, objconfig in zip(root.findall('m:resources/m:object', N), config.findall('object')):
            vertices = list(obj.find('m:mesh/m:vertices', N))
            faces = list(obj.find('m:mesh/m:triangles', N))
            newvertices, newfaces = [], []
            for volume in objconfig.findall('volume'):
                mapping = {}
                for face in faces[int(volume.get('firstid')):int(volume.get('lastid'))+1]:
                    indices = []
                    for attr in ('v1', 'v2', 'v3'):
                        vertex = vertices[int(face.get(attr))]
                        key = tuple(float(vertex.get(axis)) for axis in ('x', 'y', 'z'))
                        if key not in mapping:
                            mapping[key] = len(newvertices)
                            newvertices.append(vertex)
                        indices.append(mapping[key])
                    newfaces.append(ET.Element(tag('triangle'), dict(zip(('v1', 'v2', 'v3'), map(str, indices)))))
            vertexnode = obj.find('m:mesh/m:vertices', N)
            facenode = obj.find('m:mesh/m:triangles', N)
            vertexnode.clear()
            vertexnode.extend(newvertices)
            facenode.clear()
            facenode.extend(newfaces)
        # Keep the core meshes and build transforms, never slicer configuration.
        with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as output:
            output.writestr('[Content_Types].xml', '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>')
            output.writestr('_rels/.rels', '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>')
            output.writestr('3D/3dmodel.model', ET.tostring(root, encoding='utf-8', xml_declaration=True))


def export(output, slicer):
    output.mkdir(parents=True, exist_ok=True)
    assert not any(output.glob('*.3mf')), 'Use a fresh output directory'
    _, groups = build()
    root = ET.Element(tag('model'), {'unit': 'millimeter', 'lang': 'en-US'})
    resources = ET.SubElement(root, tag('resources'))
    buildnode = ET.SubElement(root, tag('build'))
    modelconfig = ET.Element('config')
    layout = {'shell': (12, 12), 'rear-cover': (112, 12), 'carrier': (12, 105), 'cassette': (112, 105)}
    report = {'hybrid_source': 'five functional native STEP bases plus explicit internal gussets and three removable pads',
              'settings': 'Geometry only; select printer, print and filament profiles in your slicer.',
              'mesh_method': 'CadQuery tessellation .025mm/.08rad; exact-coordinate reindex separately per named body; no tolerance welding',
              'groups': {}, 'poses': {}}
    for index, name in enumerate(NAMES, 1):
        native = groups[name]
        assembly = cq.Assembly(name=name+'-assembly')
        for label, shape in native.items():
            assert shape.isValid() and len(shape.Solids()) == 1
            assembly.add(shape, name=label)
        assembly.save(str(output/f'{name}.step'))
        clean_step(output/f'{name}.step')
        rotated = {label: pose(name, shape) for label, shape in native.items()}
        bb = bounds(cq.Compound.makeCompound(list(rotated.values())))
        tx, ty = layout[name]
        offset = (tx-bb[0], ty-bb[2], -bb[4])
        placed = {label: shape.translate(offset) for label, shape in rotated.items()}
        report['poses'][name] = {'offset': offset, 'bounds': bounds(cq.Compound.makeCompound(list(placed.values())))}
        cq.exporters.export(cq.Compound.makeCompound(list(placed.values())), str(output/f'{name}.stl'),
                            exportType='STL', tolerance=.025, angularTolerance=.08)
        obj = ET.SubElement(resources, tag('object'), {'id': str(index), 'type': 'model', 'name': name})
        mesh = ET.SubElement(obj, tag('mesh'))
        vertexnode = ET.SubElement(mesh, tag('vertices'))
        facenode = ET.SubElement(mesh, tag('triangles'))
        objconfig = ET.SubElement(modelconfig, 'object', {'id': str(index), 'instances_count': '1'})
        ET.SubElement(objconfig, 'metadata', {'type': 'object', 'key': 'name', 'value': name})
        vertices, faces = [], []
        report['groups'][name] = []
        for label, shape in placed.items():
            vv, ff = shape.tessellate(.025, .08)
            base, first = len(vertices), len(faces)
            vertices.extend(v.toTuple() for v in vv)
            faces.extend(tuple(base+k for k in face) for face in ff)
            volume = ET.SubElement(objconfig, 'volume', {'firstid': str(first), 'lastid': str(len(faces)-1)})
            ET.SubElement(volume, 'metadata', {'type': 'volume', 'key': 'name', 'value': label})
            report['groups'][name].append({'name': label, 'triangles': len(ff), 'volume_mm3': native[label].Volume()})
        for vertex in vertices:
            ET.SubElement(vertexnode, tag('vertex'), dict(zip(('x', 'y', 'z'), (format(value, '.17g') for value in vertex))))
        for face in faces:
            ET.SubElement(facenode, tag('triangle'), dict(zip(('v1', 'v2', 'v3'), map(str, face))))
        ET.SubElement(buildnode, tag('item'), {'objectid': str(index)})
    with zipfile.ZipFile(output/'geometry.3mf', 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('[Content_Types].xml', '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>')
        archive.writestr('_rels/.rels', '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>')
        archive.writestr('3D/3dmodel.model', ET.tostring(root, encoding='utf-8', xml_declaration=True))
        archive.writestr('Metadata/Slic3r_PE_model.config', ET.tostring(modelconfig))
    # Native serialization preserves body ranges for indexing; final packing drops all slicer configuration.
    command = [slicer, '--no-ensure-on-bed', '--dont-arrange', '--export-3mf',
               '--output', str(output/'native-project.3mf'), str(output/'geometry.3mf')]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (output/'native-export.log').write_bytes(result.stdout)
    assert result.returncode == 0, 'PrusaSlicer project export failed; see native-export.log'
    pack_indexed(output/'native-project.3mf', output/'display-enclosure.3mf')
    (output/'export-report.json').write_text(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--prusa-slicer', default=shutil.which('prusa-slicer') or shutil.which('PrusaSlicer'))
    args = parser.parse_args()
    assert args.prusa_slicer, 'Pass --prusa-slicer with the installed executable path'
    export(args.output.resolve(), args.prusa_slicer)
