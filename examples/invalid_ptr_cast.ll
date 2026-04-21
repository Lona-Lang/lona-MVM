; ModuleID = 'invalid_ptr_cast.c'
source_filename = "invalid_ptr_cast.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

define dso_local i32 @main() !dbg !15 {
entry:
  %1 = alloca i32, align 4
  %2 = alloca i64, align 8
  %3 = alloca ptr, align 8
  store i32 0, ptr %1, align 4
  call void @llvm.dbg.declare(metadata ptr %2, metadata !21, metadata !DIExpression()), !dbg !22
  store i64 4660, ptr %2, align 8, !dbg !22
  call void @llvm.dbg.declare(metadata ptr %3, metadata !23, metadata !DIExpression()), !dbg !24
  %4 = load i64, ptr %2, align 8, !dbg !25
  %5 = inttoptr i64 %4 to ptr, !dbg !26
  store ptr %5, ptr %3, align 8, !dbg !24
  %6 = load ptr, ptr %3, align 8, !dbg !27
  %7 = icmp ne ptr %6, null, !dbg !28
  %8 = zext i1 %7 to i32, !dbg !28
  ret i32 %8, !dbg !29
}

declare void @llvm.dbg.declare(metadata, metadata, metadata) #0

attributes #0 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!7, !8, !9, !10, !11, !12, !13}
!llvm.ident = !{!14}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "fixture", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, retainedTypes: !2, splitDebugInlining: false, nameTableKind: None)
!1 = !DIFile(filename: "invalid_ptr_cast.c", directory: "/home/lurker/workspace/compiler/lona-mvm/examples")
!2 = !{!3, !6}
!3 = !DIDerivedType(tag: DW_TAG_typedef, name: "uintptr_t", file: !4, line: 79, baseType: !5)
!4 = !DIFile(filename: "/usr/include/stdint.h", directory: "")
!5 = !DIBasicType(name: "unsigned long", size: 64, encoding: DW_ATE_unsigned)
!6 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: null, size: 64)
!7 = !{i32 7, !"Dwarf Version", i32 5}
!8 = !{i32 2, !"Debug Info Version", i32 3}
!9 = !{i32 1, !"wchar_size", i32 4}
!10 = !{i32 8, !"PIC Level", i32 2}
!11 = !{i32 7, !"PIE Level", i32 2}
!12 = !{i32 7, !"uwtable", i32 2}
!13 = !{i32 7, !"frame-pointer", i32 2}
!14 = !{!"fixture"}
!15 = distinct !DISubprogram(name: "main", scope: !16, file: !16, line: 1, type: !17, scopeLine: 1, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !20)
!16 = !DIFile(filename: "invalid_ptr_cast.c", directory: "/home/lurker/workspace/compiler/lona-mvm/examples")
!17 = !DISubroutineType(types: !18)
!18 = !{!19}
!19 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!20 = !{}
!21 = !DILocalVariable(name: "raw", scope: !15, file: !16, line: 2, type: !3)
!22 = !DILocation(line: 2, column: 15, scope: !15)
!23 = !DILocalVariable(name: "ptr", scope: !15, file: !16, line: 3, type: !6)
!24 = !DILocation(line: 3, column: 11, scope: !15)
!25 = !DILocation(line: 3, column: 25, scope: !15)
!26 = !DILocation(line: 3, column: 17, scope: !15)
!27 = !DILocation(line: 4, column: 12, scope: !15)
!28 = !DILocation(line: 4, column: 16, scope: !15)
!29 = !DILocation(line: 4, column: 5, scope: !15)
