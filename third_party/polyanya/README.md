# Polyanya reference subset

The interval heuristic and successor expansion in this directory are adapted
from Michael Cui and Daniel Harabor's reference Polyanya implementation:

- <https://bitbucket.org/dharabor/pathfinding/src/master/anyangle/polyanya/>
- reference commit: `624a6abe8777d14d0753e847b0970e74a7913b45`
- paper: <https://www.ijcai.org/proceedings/2017/70>

The retained source is licensed under the MIT terms in `LICENSE.txt`. The
reference repository's Fade2D mesh-generation utility is not used or included.
AlKanzar supplies its own conforming runtime-mesh adapter and query wrapper in
`../../src/core/navigation/Polyanya.cpp`.
