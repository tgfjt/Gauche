;;;
;;; digest - abstract base class for message digest algorithms
;;;
;;;   Copyright (c) 2002-2003 Kimura Fuyuki, All rights reserved.
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
;;;  $Id: digest.scm,v 1.6 2003-10-10 21:52:19 fuyuki Exp $
;;;

;; An abstract base- and meta-class of message digest algorithms.
;; The concrete implementation is given in modules such as
;; rfc.md5 and rfc.sha1.

(define-module util.digest
  (export <message-digest-algorithm> <message-digest-algorithm-meta>
	  digest-update! digest-final! digest digest-string
          digest-hexify)
  )
(select-module util.digest)

(define-class <message-digest-algorithm-meta> (<class>)
  ())

(define-class <message-digest-algorithm> ()
  ()
  :metaclass <message-digest-algorithm-meta>)

(define-method digest-update! ((self <message-digest-algorithm>) data)
  #f)
(define-method digest-final! ((self <message-digest-algorithm>))
  #f)
(define-method digest ((class <message-digest-algorithm-meta>))
  #f)
(define-method digest-string ((class <message-digest-algorithm-meta>) string)
  (with-input-from-string string (lambda () (digest class))))

;; utility
(define (digest-hexify string)
  (with-string-io string
    (lambda ()
      (port-for-each (lambda (x) (format #t "~2,'0x" x)) read-byte))))

(provide "util/digest")
