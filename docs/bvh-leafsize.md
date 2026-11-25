# BVH leaf size

Written by `node tools/bench-bvh.mjs`.
Best of several runs; queries are single-threaded.

- Machine: Intel(R) Core(TM) Ultra 9 386H, 16 threads, Windows_NT 10.0.26300
- Build: native-clang, RelWithDebInfo
- Date: 2026-11-25

## Result

- **Leaf size 8.** The default was 128 then, and is 8 since.
- The rule: the largest size whose worst slowdown is at most 10%. A slowdown is against the best
  size, for one query kind (coherent and random closest ray, region, sphere overlap, nearest)
  on one data set; nothing is averaged.
- **No size is within 10% everywhere.** This is the one with the smallest worst slowdown.

| leaf | worst slowdown | where |
|---|---|---|
| 4 | 1.31 | region, capsules 1000000 |
| 8 | 1.31 | nearest, triangles 10000 |
| 16 | 1.94 | nearest, triangles 10000 |
| 32 | 2.94 | nearest, triangles 10000 |
| 64 | 5.62 | nearest, triangles 10000 |
| 128 | 10.07 | coherent, capsules 1000000 |
| 256 | 24.36 | coherent, capsules 1000000 |
| 512 | 54.34 | coherent, capsules 1000000 |
| 1024 | 111.56 | coherent, capsules 1000000 |

## Data sets that decide

| source | count | leaf | coherent ray ns | random ray ns | region ns | sphere ns | nearest ns | build ms | serial build ms | refit ms | node KB |
|---|---|---|---|---|---|---|---|---|---|---|---|
| triangles | 10000 | 4 | 348 | 819 | 1922 | 4731 | 839 | 2.7 | 4.8 | 0.1 | 192 |
| triangles | 10000 | 8 | 333 | 814 | 1930 | 5690 | 1098 | 2.1 | 3.7 | 0.1 | 104 |
| triangles | 10000 | 16 | 321 | 805 | 2053 | 6734 | 1624 | 1.8 | 3.1 | 0.1 | 55 |
| triangles | 10000 | 32 | 331 | 877 | 2363 | 9006 | 2469 | 1.6 | 2.6 | 0.1 | 28 |
| triangles | 10000 | 64 | 373 | 1087 | 3020 | 12589 | 4714 | 1.4 | 2.2 | 0.1 | 14 |
| triangles | 10000 | 128 | 501 | 1579 | 4373 | 20279 | 7827 | 1.4 | 1.9 | 0.1 | 7 |
| triangles | 10000 | 256 | 752 | 2659 | 6712 | 30655 | 14672 | 1.2 | 1.5 | 0.1 | 3 |
| triangles | 10000 | 512 | 1245 | 4661 | 10726 | 52736 | 26469 | 1.1 | 1.3 | 0.1 | 2 |
| triangles | 10000 | 1024 | 2329 | 8503 | 17819 | 87852 | 51762 | 1.0 | 1.0 | 0.1 | 1 |
| triangles | 1000000 | 4 | 599 | 1503 | 13688 | 9901 | 3280 | 235.6 | 689.3 | 39.8 | 19111 |
| triangles | 1000000 | 8 | 587 | 1449 | 11965 | 11178 | 3727 | 215.2 | 597.6 | 37.9 | 10343 |
| triangles | 1000000 | 16 | 579 | 1436 | 11527 | 12822 | 4479 | 191.3 | 515.2 | 35.2 | 5496 |
| triangles | 1000000 | 32 | 632 | 1514 | 11873 | 16581 | 6494 | 180.1 | 463.5 | 32.8 | 2777 |
| triangles | 1000000 | 64 | 715 | 1708 | 13403 | 21733 | 9022 | 173.4 | 420.1 | 32.1 | 1411 |
| triangles | 1000000 | 128 | 932 | 2141 | 17377 | 31593 | 14182 | 173.2 | 385.9 | 31.5 | 705 |
| triangles | 1000000 | 256 | 1355 | 3080 | 23107 | 46773 | 23521 | 166.1 | 349.0 | 31.2 | 353 |
| triangles | 1000000 | 512 | 2256 | 4955 | 33962 | 70121 | 39877 | 165.9 | 325.9 | 31.3 | 178 |
| triangles | 1000000 | 1024 | 4038 | 8706 | 51356 | 106969 | 68390 | 160.1 | 293.4 | 32.4 | 89 |
| capsules | 1000000 | 4 | 766 | 1824 | 8839 | 6404 | 1285 | 219.5 | 677.0 | 46.3 | 19887 |
| capsules | 1000000 | 8 | 808 | 1902 | 8118 | 6327 | 1292 | 192.0 | 562.1 | 41.7 | 10786 |
| capsules | 1000000 | 16 | 945 | 2183 | 6724 | 6637 | 1422 | 172.1 | 468.5 | 39.5 | 5010 |
| capsules | 1000000 | 32 | 1501 | 3264 | 6867 | 7409 | 1800 | 159.2 | 421.4 | 36.4 | 2735 |
| capsules | 1000000 | 64 | 3142 | 6402 | 7672 | 8948 | 2596 | 154.3 | 385.8 | 37.8 | 1435 |
| capsules | 1000000 | 128 | 7713 | 15034 | 9253 | 11554 | 4163 | 149.3 | 353.9 | 36.8 | 725 |
| capsules | 1000000 | 256 | 18662 | 34310 | 12698 | 16510 | 7249 | 152.5 | 321.7 | 36.4 | 361 |
| capsules | 1000000 | 512 | 41632 | 76111 | 18375 | 25492 | 12720 | 138.0 | 297.7 | 37.6 | 179 |
| capsules | 1000000 | 1024 | 85463 | 158600 | 28101 | 40876 | 23381 | 140.1 | 273.7 | 37.2 | 89 |
| spheres | 1000000 | 4 | 1717 | 4170 | 34887 | 1419 | 1128 | 264.7 | 796.4 | 84.0 | 20688 |
| spheres | 1000000 | 8 | 1752 | 4054 | 32187 | 1292 | 1005 | 233.5 | 682.8 | 84.8 | 10909 |
| spheres | 1000000 | 16 | 2008 | 4359 | 30770 | 1146 | 980 | 213.5 | 599.4 | 74.2 | 5555 |
| spheres | 1000000 | 32 | 2499 | 5301 | 33400 | 1172 | 1003 | 203.4 | 544.9 | 77.5 | 2780 |
| spheres | 1000000 | 64 | 3475 | 6967 | 39857 | 1387 | 1215 | 195.7 | 506.1 | 74.4 | 1370 |
| spheres | 1000000 | 128 | 5238 | 10493 | 51885 | 1941 | 1627 | 190.3 | 459.9 | 72.4 | 665 |
| spheres | 1000000 | 256 | 8508 | 16137 | 65569 | 2896 | 2606 | 178.1 | 432.4 | 74.6 | 316 |
| spheres | 1000000 | 512 | 14315 | 26554 | 96669 | 5042 | 4156 | 180.8 | 401.8 | 75.7 | 146 |
| spheres | 1000000 | 1024 | 24408 | 45633 | 137424 | 9167 | 7885 | 181.7 | 366.2 | 72.4 | 68 |

## Confirmation

- At 10M triangles, against its neighbours, size 8's worst slowdown is 1.15 (region, triangles 9998244): **the choice is not confirmed**.

| source | count | leaf | coherent ray ns | random ray ns | region ns | sphere ns | nearest ns | build ms | serial build ms | refit ms | node KB |
|---|---|---|---|---|---|---|---|---|---|---|---|
| triangles | 9998244 | 4 | 787 | 2206 | 75403 | 50089 | 9554 | 2075.8 | 8216.6 | 626.2 | 190927 |
| triangles | 9998244 | 8 | 753 | 2134 | 60222 | 51370 | 10397 | 1797.2 | 7153.9 | 576.2 | 103393 |
| triangles | 9998244 | 16 | 739 | 2110 | 52372 | 54638 | 12818 | 1619.4 | 6384.8 | 535.5 | 54926 |

## Notes

- **The default is 8,** although neither run found a size within 10% everywhere, and 10M triangles don't confirm
  it: that is the rule's fallback, the size with the smallest worst slowdown. The P3 plan has the reasons.
- **In wasm** (`--wasm`: the same data sets without 10M; the same machine, wasm-st, RelWithDebInfo, node v24.22.1,
  2026-11-25) it is 8 as well, also without a size within 10%:

| leaf | worst slowdown | where |
|---|---|---|
| 4 | 1.37 | sphere, spheres 1000000 |
| 8 | 1.27 | sphere, spheres 1000000 |
| 16 | 1.29 | nearest, triangles 10000 |
| 32 | 1.92 | nearest, triangles 10000 |
| 64 | 2.64 | nearest, triangles 10000 |
| 128 | 4.12 | nearest, triangles 10000 |
| 256 | 8.84 | coherent, capsules 1000000 |
| 512 | 18.71 | coherent, capsules 1000000 |
| 1024 | 39.05 | coherent, capsules 1000000 |
