// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace cpu12::test {
[[gnu::noinline]] inline void clobber_a64(const void *aligned) noexcept {
  asm volatile("mov x16,sp\n"
               "tst x16,#15\n"
               "b.eq 1f\n"
               "brk #0\n"
               "1:\n"
               "ldr q0,[%0]\n"
               "mov x0,xzr\n"
               "mov x1,xzr\n"
               "mov x2,xzr\n"
               "mov x3,xzr\n"
               "mov x4,xzr\n"
               "mov x5,xzr\n"
               "mov x6,xzr\n"
               "mov x7,xzr\n"
               "mov x8,xzr\n"
               "mov x9,xzr\n"
               "mov x10,xzr\n"
               "mov x11,xzr\n"
               "mov x12,xzr\n"
               "mov x13,xzr\n"
               "mov x14,xzr\n"
               "mov x15,xzr\n"
               "mov x16,xzr\n"
               "mov x17,xzr\n"
               "movi v0.2d,#0\n"
               "movi v1.2d,#0\n"
               "movi v2.2d,#0\n"
               "movi v3.2d,#0\n"
               "movi v4.2d,#0\n"
               "movi v5.2d,#0\n"
               "movi v6.2d,#0\n"
               "movi v7.2d,#0\n"
               "movi v8.2d,#0\n"
               "movi v9.2d,#0\n"
               "movi v10.2d,#0\n"
               "movi v11.2d,#0\n"
               "movi v12.2d,#0\n"
               "movi v13.2d,#0\n"
               "movi v14.2d,#0\n"
               "movi v15.2d,#0\n"
               "movi v16.2d,#0\n"
               "movi v17.2d,#0\n"
               "movi v18.2d,#0\n"
               "movi v19.2d,#0\n"
               "movi v20.2d,#0\n"
               "movi v21.2d,#0\n"
               "movi v22.2d,#0\n"
               "movi v23.2d,#0\n"
               "movi v24.2d,#0\n"
               "movi v25.2d,#0\n"
               "movi v26.2d,#0\n"
               "movi v27.2d,#0\n"
               "movi v28.2d,#0\n"
               "movi v29.2d,#0\n"
               "movi v30.2d,#0\n"
               "movi v31.2d,#0\n"
               :
               : "r"(aligned)
               : "x0",
                 "x1",
                 "x2",
                 "x3",
                 "x4",
                 "x5",
                 "x6",
                 "x7",
                 "x8",
                 "x9",
                 "x10",
                 "x11",
                 "x12",
                 "x13",
                 "x14",
                 "x15",
                 "x16",
                 "x17",
                 "v0",
                 "v1",
                 "v2",
                 "v3",
                 "v4",
                 "v5",
                 "v6",
                 "v7",
                 "v8",
                 "v9",
                 "v10",
                 "v11",
                 "v12",
                 "v13",
                 "v14",
                 "v15",
                 "v16",
                 "v17",
                 "v18",
                 "v19",
                 "v20",
                 "v21",
                 "v22",
                 "v23",
                 "v24",
                 "v25",
                 "v26",
                 "v27",
                 "v28",
                 "v29",
                 "v30",
                 "v31",
                 "cc",
                 "memory");
}
}
