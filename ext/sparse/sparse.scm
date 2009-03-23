;;;
;;; util.sparse - sparse data structures
;;;  
;;;   Copyright (c) 2007-2009  Shiro Kawai  <shiro@acm.org>
;;;   
;;;   Redistribution and use in source and binary forms, with or without
;;;   modification, are permitted provided that the following conditions
;;;   are met:
;;;   
;;;   1. Redistributions of source code must retain the above copyright
;;;      notice, this list of conditions and the following disclaimer.
;;;  
;;;   2. Redistributions in binary form must reproduce the above copyright
;;;      notice, this list of conditions and the following disclaimer in the
;;;      documentation and/or other materials provided with the distribution.
;;;  
;;;   3. Neither the name of the authors nor the names of its contributors
;;;      may be used to endorse or promote products derived from this
;;;      software without specific prior written permission.
;;;  
;;;   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
;;;   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
;;;   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
;;;   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
;;;   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
;;;   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
;;;   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
;;;   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
;;;   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
;;;   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
;;;   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
;;;  
;;;  $Id: fcntl.scm,v 1.6 2007/03/02 07:39:05 shirok Exp $
;;;


(define-module util.sparse
  (export <sparse-vector> make-sparse-vector sparse-vector-num-entries
          sparse-vector-ref sparse-vector-set!
          sparse-vector-clear! %sparse-vector-dump
          <sparse-table> make-sparse-table sparse-table-num-entries
          sparse-table-ref sparse-table-set!
          sparse-table-clear! sparse-table-delete!
          sparse-table-fold sparse-table-map sparse-table-for-each
          sparse-table-keys sparse-table-values %sparse-table-dump)
  )
(select-module util.sparse)

(inline-stub
 "#include \"ctrie.h\""
 "#include \"spvec.h\""
 "#include \"sptab.h\""
 )

;; Sparse vectors
(inline-stub
 (initcode "Scm_Init_spvec(mod);")

 (define-type <sparse-vector> "SparseVector*" "sparse vector"
   "SPARSE_VECTOR_P" "SPARSE_VECTOR")

 (define-cproc make-sparse-vector ()
   (result (MakeSparseVector 0)))

 (define-cproc sparse-vector-num-entries (sv::<sparse-vector>) ::<ulong>
   (result (-> sv numEntries)))
 
 (define-cproc sparse-vector-ref
   (sv::<sparse-vector> index::<ulong> :optional fallback)
   SparseVectorRef)

 (define-cproc sparse-vector-set!
   (sv::<sparse-vector> index::<ulong> value) ::<void>
   SparseVectorSet)

 (define-cproc sparse-vector-clear! (sv::<sparse-vector>) ::<void>
   SparseVectorClear)

 (define-cproc %sparse-vector-dump (sv::<sparse-vector>) ::<void>
   SparseVectorDump)
 )

;; Sparse hashtables
(inline-stub
 (initcode "Scm_Init_sptab(mod);")

 (define-type <sparse-table> "SparseTable*" "sparse table"
   "SPARSE_TABLE_P" "SPARSE_TABLE")

 (define-cproc make-sparse-table (type)
   (let* ([t::ScmHashType SCM_HASH_EQ])
     (cond
      [(SCM_EQ type 'eq?)      (set! t SCM_HASH_EQ)]
      [(SCM_EQ type 'eqv?)     (set! t SCM_HASH_EQV)]
      [(SCM_EQ type 'equal?)   (set! t SCM_HASH_EQUAL)]
      [(SCM_EQ type 'string=?) (set! t SCM_HASH_STRING)]
      [else (Scm_Error "unsupported sparse-table hash type: %S" type)])
     (result (MakeSparseTable t 0))))

 (define-cproc sparse-table-num-entries (st::<sparse-table>) ::<ulong>
   (result (-> st numEntries)))
 
 (define-cproc sparse-table-ref (st::<sparse-table> key :optional fallback)
   (let* ([r (SparseTableRef st key fallback)])
     (when (SCM_UNBOUNDP r)
       (Scm_Error "%S doesn't have an entry for key %S" (SCM_OBJ st) key))
     (result r)))

 (define-cproc sparse-table-set! (st::<sparse-table> key value)
   (result (SparseTableSet st key value 0)))

 (define-cproc sparse-table-delete! (st::<sparse-table> key) ::<boolean>
   (result (not (SCM_UNBOUNDP (SparseTableDelete st key)))))

 (define-cproc sparse-table-clear! (st::<sparse-table>) ::<void>
   SparseTableClear)

 (define-cfn sparse-table-iter (args::ScmObj* nargs::int data::void*) :static
   (let* ([iter::SparseTableIter* (cast SparseTableIter* data)]
          [r (SparseTableIterNext iter)]
          [eofval (aref args 0)])
     (if (SCM_FALSEP r)
       (return (values eofval eofval))
       (return (values (SCM_CAR r) (SCM_CDR r))))))

 (define-cproc %sparse-table-iter (st::<sparse-table>)
   (let* ([iter::SparseTableIter* (SCM_NEW SparseTableIter)])
     (SparseTableIterInit iter st)
     (result (Scm_MakeSubr sparse-table-iter iter 1 0 '"sparse-table-iterator"))))

 (define-cproc %sparse-table-dump (st::<sparse-table>) ::<void>
   SparseTableDump)
 )

(define (sparse-table-fold st proc seed)
  (let ([iter (%sparse-table-iter st)]
        [end  (list)])
    (let loop ((seed seed))
      (receive (key val) (iter end)
        (if (eq? key end)
          seed
          (loop (proc key val seed)))))))

(define (sparse-table-map st proc)
  (sparse-table-fold st (lambda (k v s) (cons (proc k v) s)) '()))

(define (sparse-table-for-each st proc)
  (sparse-table-fold st (lambda (k v _) (proc k v)) #f))

(define (sparse-table-keys st)
  (sparse-table-fold st (lambda (k v s) (cons k s)) '()))

(define (sparse-table-values st)
  (sparse-table-fold st (lambda (k v s) (cons v s)) '()))

(provide "util/sparse")