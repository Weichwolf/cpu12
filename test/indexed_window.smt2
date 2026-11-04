; SPDX-License-Identifier: GPL-3.0-only
; Independent interval expectations for emitted indexed byte/word RAM guards.
; Expected results: unsat, unsat, unsat, unsat, sat, sat.
; This proves address arithmetic only, not x64 encoding or CPU/firmware behavior.
(set-logic QF_BV)
(declare-fun address () (_ BitVec 16))
(declare-fun first () (_ BitVec 16))
(declare-fun words () (_ BitVec 32))
(declare-fun displacement () (_ BitVec 16))
(declare-fun length () (_ BitVec 32))
(define-fun a () (_ BitVec 32) ((_ zero_extend 16) address))
(define-fun begin () (_ BitVec 32) ((_ zero_extend 16) first))
(define-fun end () (_ BitVec 32) (bvadd begin words))
(assert (bvule words (bvsub #x00010000 begin)))
(define-fun in-range ((value (_ BitVec 32))) Bool
  (and (bvuge value begin) (bvult value end)))
(define-fun high-offset () (_ BitVec 32) (bvsub a begin))
(define-fun high-guard () Bool (bvult high-offset words))

(push)
(assert (not (= high-guard (in-range a))))
(check-sat)
(pop)

(define-fun low16 () (_ BitVec 16) (bvadd address #x0001))
(define-fun low () (_ BitVec 32) ((_ zero_extend 16) low16))
(define-fun low-offset () (_ BitVec 32) (bvsub low begin))
(define-fun low-guard () Bool (bvult low-offset words))
(define-fun word-guard () Bool (and high-guard low-guard))
(define-fun intended-low () (_ BitVec 32)
  (ite (= a #x0000ffff) #x00000000 (bvadd a #x00000001)))
(push)
(assert (not (= word-guard (and (in-range a) (in-range intended-low)))))
(check-sat)
(pop)

(define-fun full () Bool (and (= begin #x00000000) (= words #x00010000)))
(define-fun native-high () (_ BitVec 32) (ite full a high-offset))
(push)
(assert word-guard)
(assert (or (bvuge native-high words) (bvuge low-offset words)))
(check-sat)
(pop)

; Folding the instruction length into a constant PC-relative displacement.
(push)
(assert (and (bvuge length #x00000002) (bvule length #x00000004)))
(define-fun folded () (_ BitVec 32)
  (bvand (bvadd ((_ zero_extend 16) displacement) length) #x0000ffff))
(define-fun emitted () (_ BitVec 16) ((_ extract 15 0) (bvadd a folded)))
(define-fun expected () (_ BitVec 16)
  (bvadd (bvadd address ((_ extract 15 0) length)) displacement))
(assert (not (= emitted expected)))
(check-sat)
(pop)

; Negative controls: signed window comparison and a missing word wrap are wrong.
(push)
(assert (not (= (bvslt high-offset words) (in-range a))))
(check-sat)
(pop)
(push)
(assert (not (= word-guard
  (and high-guard (bvult (bvsub (bvadd a #x00000001) begin) words)))))
(check-sat)
(pop)
