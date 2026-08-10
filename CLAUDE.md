XenBlocks is a mining algorithm and protocol on X1 Network. 
- The official website is xenblocks.io
- The most popular and efficient miner is the Woody Miner from https://www.woodyminer.com/

Githubs and Docs 
- The project found github is https://github.com/jacklevin74/xenminer
- The Woody Miner Github is https://github.com/woodysoil/XenblocksMiner 
- The Jozef Miner Github is https://github.com/JozefJarosciak/xgpu
- Official Gitbook is https://docs.xenblocks.io/


We are looking to revisit both miners and create a new. 

One issue with the current miner is that the central server goes offline. 

We should be able to store the hashes and timestamps locally then resubmit when server is online. 

## Project layout

- `repos/` — local clones of the three reference miners (git-ignored, research only):
  - `repos/xenminer` — jacklevin74/xenminer (reference Python miner + server side)
  - `repos/XenblocksMiner` — woodysoil/XenblocksMiner (Woody Miner, C++/CUDA)
  - `repos/xgpu` — JozefJarosciak/xgpu (Jozef Miner, cloud deployment scripts)
- `docs/` — our research documentation, numbered by source:
  - `01-xenminer-reference.md`, `02-xenblocksminer-woody.md`, `03-xgpu-jozef.md`,
    `04-official-docs-ecosystem.md`, `05-synthesis-new-miner-design.md`,
    `06-optimization-plan.md`, `07-external-reviews.md`
  - `docs/reviews/` — verbatim review docs from other models (Kimmy, Grok, Sol)
- `research/` — scratch notes, experiments, validation records
- `treeminer/` — the miner itself (fork of XenblocksMiner). `treeminer/PLAN.md` is the
  design authority; `treeminer/CHANGES-FROM-UPSTREAM.md` is the divergence log.
  TreeMiner components: `src/treeminer/` (shared contract), `src/journal/`,
  `src/submit/`, tests in `tests/`.

## Key design goal

Offline-resilient submission: persist found hashes + timestamps locally (durable queue),
resubmit automatically when the central server is reachable again. Before building,
confirm from the reference server code whether delayed submissions are accepted
(server-side timestamp validation is the critical unknown).
