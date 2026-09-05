#!/usr/bin/env python3
"""Pack binary STL parts into one 3MF (millimetres) with per-part print
orientation and a simple bed layout. No dependencies.

usage: stl_to_3mf.py OUT.3mf NAME=path.stl:ORIENT[:X,Y] ...
ORIENT: asis | flipx (180 deg about X, i.e. face-down for the Top / plate)
        | outer-x (rotate so the part's -X face points down, for the power cap)
        | outer-rz<deg> (rotate about Z by <deg> first, then like outer-x)
X,Y: bed offset in mm for the part's bounding-box centre (default: auto row).
"""
import math, struct, sys, zipfile, xml.sax.saxutils as sx

def read_stl(path):
    data = open(path, 'rb').read()
    n = struct.unpack('<I', data[80:84])[0]
    tris = []
    for i in range(n):
        v = struct.unpack('<12f', data[84 + i*50: 84 + i*50 + 48])
        tris.append(((v[3], v[4], v[5]), (v[6], v[7], v[8]), (v[9], v[10], v[11])))
    return tris

def rot(axis, deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    if axis == 'x': return ((1,0,0),(0,c,-s),(0,s,c))
    if axis == 'y': return ((c,0,s),(0,1,0),(-s,0,c))
    return ((c,-s,0),(s,c,0),(0,0,1))

def mul(a, b):
    return tuple(tuple(sum(a[i][k]*b[k][j] for k in range(3)) for j in range(3)) for i in range(3))

def apply(m, p):
    return tuple(sum(m[i][k]*p[k] for k in range(3)) for i in range(3))

def orient_matrix(spec):
    if spec == 'asis': return ((1,0,0),(0,1,0),(0,0,1))
    if spec == 'flipx': return rot('x', 180)
    if spec == 'outer-x': return rot('y', -90)
    if spec.startswith('outer-rz'):
        return mul(rot('y', -90), rot('z', float(spec[8:])))
    raise SystemExit('unknown orient ' + spec)

def main():
    out = sys.argv[1]
    objects = []
    cursor_x = 0.0
    for arg in sys.argv[2:]:
        name, rest = arg.split('=', 1)
        parts = rest.split(':')
        path, spec = parts[0], parts[1]
        m = orient_matrix(spec)
        tris = [tuple(apply(m, p) for p in t) for t in read_stl(path)]
        # rotation flips winding when det<0 (never here: pure rotations)
        xs = [p[0] for t in tris for p in t]; ys = [p[1] for t in tris for p in t]; zs = [p[2] for t in tris for p in t]
        cx, cy, z0 = (min(xs)+max(xs))/2, (min(ys)+max(ys))/2, min(zs)
        if len(parts) > 2:
            bx, by = map(float, parts[2].split(','))
        else:
            w = max(xs) - min(xs)
            bx, by = cursor_x + w/2, 0.0
            cursor_x += w + 8.0
        objects.append((name, tris, (bx - cx, by - cy, -z0)))
    # write model
    lines = ['<?xml version="1.0" encoding="UTF-8"?>',
             '<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">',
             '<resources>']
    for i, (name, tris, _) in enumerate(objects, start=1):
        verts = {}; vlist = []; faces = []
        for t in tris:
            idx = []
            for p in t:
                key = (round(p[0], 4), round(p[1], 4), round(p[2], 4))
                if key not in verts:
                    verts[key] = len(vlist); vlist.append(key)
                idx.append(verts[key])
            if len(set(idx)) == 3: faces.append(idx)
        lines.append('<object id="%d" name="%s" type="model"><mesh><vertices>' % (i, sx.escape(name)))
        lines += ['<vertex x="%g" y="%g" z="%g"/>' % v for v in vlist]
        lines.append('</vertices><triangles>')
        lines += ['<triangle v1="%d" v2="%d" v3="%d"/>' % tuple(f) for f in faces]
        lines.append('</triangles></mesh></object>')
        print('%-14s verts %6d tris %6d' % (name, len(vlist), len(faces)))
    lines.append('</resources><build>')
    for i, (_, _, (tx, ty, tz)) in enumerate(objects, start=1):
        lines.append('<item objectid="%d" transform="1 0 0 0 1 0 0 0 1 %g %g %g"/>' % (i, tx, ty, tz))
    lines.append('</build></model>')
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('[Content_Types].xml', '<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>')
        z.writestr('_rels/.rels', '<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>')
        z.writestr('3D/3dmodel.model', '\n'.join(lines))
    print('wrote', out)

if __name__ == '__main__':
    main()
