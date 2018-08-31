; RUN: opt < %s -instcombine -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64"

; CHECK: define i1 @compare(i32* %a, i32* %b)
define i1 @compare(i32* %a, i32* %b) {
  %sub = call i64 @llvm.psub.i64.p0i32.p0i32(i32* %a, i32* %b)
; CHECK: %cmp = icmp eq i32* %a, %b
  %cmp = icmp eq i64 %sub, 0
  ret i1 %cmp
}

; CHECK: define i1 @compare2(i32* %a, i32* %b)
define i1 @compare2(i32* %a, i32 *%b) {
  %sub = call i64 @llvm.psub.i64.p0i32.p0i32(i32* %a, i32* %b)
; CHECK: %cmp = icmp ne i32* %a, %b
  %cmp = icmp ne i64 %sub, 0
  ret i1 %cmp
}

define i64 @ptrdiff(i8* %a, i64 %b) {
  %a2 = getelementptr i8, i8* %a, i64 %b
  %sub = call i64 @llvm.psub.i64.p0i8.p0i8(i8* %a2, i8* %a)
; CHECK: ret i64 %b
  ret i64 %sub
}

declare i64 @llvm.psub.i64.p0i32.p0i32(i32*, i32*) #0
declare i64 @llvm.psub.i64.p0i8.p0i8(i8*, i8*) #0

attributes #0 = { nounwind readnone speculatable norecurse }
