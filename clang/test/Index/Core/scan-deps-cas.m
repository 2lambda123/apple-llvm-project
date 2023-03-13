// RUN: rm -rf %t.mcp %t

// RUN: c-index-test core --scan-deps %S -output-dir=%t -cas-path %t/cas \
// RUN:  -- %clang -c -I %S/Inputs/module \
// RUN:     -fmodules -fmodules-cache-path=%t.mcp \
// RUN:     -o FoE.o -x objective-c %s > %t.result
// RUN: cat %t.result | sed 's/\\/\//g' | FileCheck %s -DPREFIX=%S -DOUTPUTS=%/t

// RUN: env CLANG_CACHE_USE_CASFS_DEPSCAN=1 c-index-test core --scan-deps %S -output-dir=%t -cas-path %t/cas \
// RUN:  -- %clang -c -I %S/Inputs/module \
// RUN:     -fmodules -fmodules-cache-path=%t.mcp \
// RUN:     -o FoE.o -x objective-c %s > %t.casfs.result
// RUN: cat %t.casfs.result | sed 's/\\/\//g' | FileCheck %s -DPREFIX=%S -DOUTPUTS=%/t

// FIXME: enable modules when supported.
// RUN: env CLANG_CACHE_USE_INCLUDE_TREE=1 c-index-test core --scan-deps %S -output-dir=%t -cas-path %t/cas \
// RUN:  -- %clang -c -I %S/Inputs/module \
// RUN:     -o FoE.o -x objective-c %s > %t.includetree.result
// RUN: cat %t.includetree.result | sed 's/\\/\//g' | FileCheck %s -DPREFIX=%S -DOUTPUTS=%/t -check-prefix=INCLUDE_TREE

// RUN: c-index-test core --scan-deps %S -output-dir=%t \
// RUN:  -- %clang -c -I %S/Inputs/module \
// RUN:     -fmodules -fmodules-cache-path=%t.mcp \
// RUN:     -o FoE.o -x objective-c %s | FileCheck %s -check-prefix=NO_CAS
// NO_CAS-NOT: fcas
// NO_CAS-NOT: faction-cache
// NO_CAS-NOT: fcache-compile-job

#include "ModA.h"

// CHECK:       modules:
// CHECK-NEXT:   module:
// CHECK-NEXT:     name: ModA
// CHECK-NEXT:     context-hash: [[HASH_MOD_A:[A-Z0-9]+]]
// CHECK-NEXT:     module-map-path: [[PREFIX]]/Inputs/module/module.modulemap
// CHECK-NEXT:     module-deps:
// CHECK-NEXT:     file-deps:
// CHECK-NEXT:       [[PREFIX]]/Inputs/module/ModA.h
// CHECK-NEXT:       [[PREFIX]]/Inputs/module/SubModA.h
// CHECK-NEXT:       [[PREFIX]]/Inputs/module/SubSubModA.h
// CHECK-NEXT:       [[PREFIX]]/Inputs/module/module.modulemap
// CHECK-NEXT:     build-args:
// CHECK-SAME:       -cc1
// CHECK-SAME:       -fcas-path
// CHECK-SAME:       -fcas-fs llvmcas://{{[[:xdigit:]]+}}
// CHECK-SAME:       -fcache-compile-job
// CHECK-SAME:       -emit-module
// CHECK-SAME:       -fmodule-name=ModA
// CHECK-SAME:       -fno-implicit-modules

// CHECK-NEXT: dependencies:
// CHECK-NEXT:   command 0:
// CHECK-NEXT:     context-hash: [[HASH_TU:[A-Z0-9]+]]
// CHECK-NEXT:     module-deps:
// CHECK-NEXT:       ModA:[[HASH_MOD_A]]
// CHECK-NEXT:     file-deps:
// CHECK-NEXT:       [[PREFIX]]/scan-deps-cas.m
// CHECK-NEXT:     build-args:
// CHECK-SAME:       -cc1
// CHECK-SAME:       -fcas-path
// CHECK-SAME:       -fcas-fs llvmcas://{{[[:xdigit:]]+}}
// CHECK-SAME:       -fcache-compile-job
// CHECK-SAME:       -fmodule-file-cache-key [[PCM:.*ModA_.*pcm]] llvmcas://{{[[:xdigit:]]+}}
// CHECK-SAME:       -fmodule-file={{(ModA=)?}}[[PCM]]

// INCLUDE_TREE:      dependencies:
// INCLUDE_TREE-NEXT:   command 0:
// INCLUDE_TREE-NEXT:     context-hash: [[HASH_TU:[A-Z0-9]+]]
// INCLUDE_TREE-NEXT:     module-deps:
// INCLUDE_TREE-NEXT:     file-deps:
// INCLUDE_TREE-NEXT:       [[PREFIX]]/scan-deps-cas.m
// INCLUDE_TREE-NEXT:       [[PREFIX]]/Inputs/module/ModA.h
// INCLUDE_TREE-NEXT:     build-args:
// INCLUDE_TREE-SAME:       -cc1
// INCLUDE_TREE-SAME:       -fcas-path
// INCLUDE_TREE-SAME:       -fcas-include-tree llvmcas://{{[[:xdigit:]]+}}
// INCLUDE_TREE-SAME:       -fcache-compile-job
