# CDT

This directory vendors the header-only subset of [CDT](https://github.com/artem-ogre/CDT),
a constrained Delaunay triangulation implementation used by the navigation-mesh baker.

- upstream commit: `763e8262dbd3b9493a4c5ac528461074f6d9574d`
- license: Mozilla Public License 2.0; see `LICENSE`
- the upstream source files are unmodified

AlKanzar-specific boundary extraction and navmesh adaptation stay in
`src/core/navigation/NavigationDelaunayRemesh.cpp`.
