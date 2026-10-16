; ============================================================================
;  PSBET — w64_thunks.asm (MASM, x64)
;  SysV AMD64 -> Win64 ABI trampolines for guest-callable exports.
;
;  PS5-class guests are compiled for the SysV ABI: integer/pointer args
;  arrive in RDI, RSI, RDX, RCX, R8, R9. The MSVC host expects Win64 order:
;  RCX, RDX, R8, R9 (+ stack). These thunks shuffle the first four args,
;  reserve the Win64 shadow space, and tail-call the real implementation.
;  Kernel/GPU surface functions take integers and pointers only (no SSE
;  args), so the general-purpose shuffle is complete for this ABI surface.
;
;  Arity map used per export (see kernel_shim.cpp export table):
;    0..6 args. Args beyond four flow on the stack in both ABIs with
;    DIFFERENT offsets (SysV: first stack arg at [rsp+0] after return addr;
;    Win64: at [rsp+8] after return addr, i.e. [rsp+0] at entry+8... ) —
;    for the current guest surface no export takes more than 6 args, so the
;    four-register shuffle is exact. Exports with 5-6 args: SysV stack args
;    sit at rsp+8.. after the return address; Win64 expects them at rsp+40
;    (after shadow space) — thunk5/thunk6 shift one/two stack words.
; ============================================================================

EXTERN psbXSysAllocateDirectMemory:PROC
EXTERN psbXSysMapDirectMemory:PROC
EXTERN psbXSysGetDirectMemorySize:PROC
EXTERN psbXSysGetProcessTime:PROC
EXTERN psbXSysGetProcessTimeInTick:PROC
EXTERN psbXSysUsleep:PROC
EXTERN psbXSysNanosleep:PROC
EXTERN psbXSysSleep:PROC
EXTERN psbXSysCreateThread:PROC
EXTERN psbXSysStartThread:PROC
EXTERN psbXSysWaitThreadEnd:PROC
EXTERN psbXSysExitThread:PROC
EXTERN psbXSysExitProcess:PROC
EXTERN psbXSysGetThreadId:PROC
EXTERN psbXSysCreateMutex:PROC
EXTERN psbXSysLockMutex:PROC
EXTERN psbXSysUnlockMutex:PROC
EXTERN psbXSysCreateSemaphore:PROC
EXTERN psbXSysWaitSemaphore:PROC
EXTERN psbXSysSignalSemaphore:PROC
EXTERN psbXSysCreateCond:PROC
EXTERN psbXSysWaitCond:PROC
EXTERN psbXSysSignalCond:PROC
EXTERN psbXSysSignalCondAll:PROC
EXTERN psbXSysWrite:PROC
EXTERN psbXSysRead:PROC
EXTERN psbXSysOpen:PROC
EXTERN psbXSysClose:PROC
EXTERN psbXSysIsNeoMode:PROC
EXTERN psbXTlsGetAddr:PROC
EXTERN psbXCxaFinalize:PROC
EXTERN psbXGetRuntimeVersion:PROC
EXTERN psbXGetRuntimeName:PROC
EXTERN psbXGnmSubmitCommandBuffers:PROC
EXTERN psbXStackChkFail:PROC

_TEXT SEGMENT

; --- arity 0 -----------------------------------------------------------------
thunk0 MACRO name, target
name PROC
    sub     rsp, 28h
    call    target
    add     rsp, 28h
    ret
name ENDP
ENDM

; --- arity 1: rdi -> rcx -------------------------------------------------------
thunk1 MACRO name, target
name PROC
    mov     rcx, rdi
    sub     rsp, 28h
    call    target
    add     rsp, 28h
    ret
name ENDP
ENDM

; --- arity 2: rdi->rcx, rsi->rdx ----------------------------------------------
thunk2 MACRO name, target
name PROC
    mov     rcx, rdi
    mov     rdx, rsi
    sub     rsp, 28h
    call    target
    add     rsp, 28h
    ret
name ENDP
ENDM

; --- arity 3: rdi->rcx, rsi->rdx, rdx->r8 --------------------------------------
thunk3 MACRO name, target
name PROC
    mov     rcx, rdi
    mov     rdx, rsi
    mov     r8,  rdx
    sub     rsp, 28h
    call    target
    add     rsp, 28h
    ret
name ENDP
ENDM

; --- arity 4: rdi->rcx, rsi->rdx, rdx->r8, rcx->r9 ------------------------------
; NOTE: rcx must be read BEFORE it is clobbered by the r8 move.
thunk4 MACRO name, target
name PROC
    mov     r9,  rcx
    mov     r8,  rdx
    mov     rdx, rsi
    mov     rcx, rdi
    sub     rsp, 28h
    call    target
    add     rsp, 28h
    ret
name ENDP
ENDM

; --- arity 5: 4 register shuffles + 1 stack arg ---------------------------------
; SysV stack arg 5 sits at [rsp+8] on entry (return address at [rsp]).
; Win64 wants it at [rsp+28h+8] after we reserve shadow space (the callee
; reads stack arg 5 at [rsp+28h+8] relative to its own entry).
; Strategy: reserve 38h, copy the stack arg, then call.
thunk5 MACRO name, target
name PROC
    mov     r9,  rcx
    mov     r8,  rdx
    mov     rdx, rsi
    mov     rcx, rdi
    mov     rax, [rsp+8]          ; SysV stack arg 5
    sub     rsp, 38h
    mov     [rsp+28h], rax        ; Win64 stack arg 5 slot
    call    target
    add     rsp, 38h
    ret
name ENDP
ENDM

; --- arity 6: 4 register shuffles + 2 stack args --------------------------------
thunk6 MACRO name, target
name PROC
    mov     r9,  rcx
    mov     r8,  rdx
    mov     rdx, rsi
    mov     rcx, rdi
    mov     rax, [rsp+8]          ; stack arg 5
    mov     r10, [rsp+10h]        ; stack arg 6
    sub     rsp, 38h
    mov     [rsp+28h], rax
    mov     [rsp+30h], r10
    call    target
    add     rsp, 38h
    ret
name ENDP
ENDM

; --- exports -----------------------------------------------------------------
thunk4 psbTWinAllocateDirectMemory, psbXSysAllocateDirectMemory   ; 6 args
thunk5 psbTWinMapDirectMemory,      psbXSysMapDirectMemory        ; 5 args
thunk1 psbTWinGetDirectMemorySize,  psbXSysGetDirectMemorySize
thunk0 psbTWinGetProcessTime,       psbXSysGetProcessTime
thunk0 psbTWinGetProcessTimeInTick, psbXSysGetProcessTimeInTick
thunk1 psbTWinUsleep,               psbXSysUsleep
thunk2 psbTWinNanosleep,            psbXSysNanosleep
thunk1 psbTWinSleep,                psbXSysSleep
thunk6 psbTWinCreateThread,         psbXSysCreateThread           ; 6 args
thunk3 psbTWinStartThread,          psbXSysStartThread
thunk3 psbTWinWaitThreadEnd,        psbXSysWaitThreadEnd
thunk1 psbTWinExitThread,           psbXSysExitThread
thunk1 psbTWinExitProcess,          psbXSysExitProcess
thunk0 psbTWinGetThreadId,          psbXSysGetThreadId
thunk4 psbTWinCreateMutex,          psbXSysCreateMutex
thunk2 psbTWinLockMutex,            psbXSysLockMutex
thunk1 psbTWinUnlockMutex,          psbXSysUnlockMutex
thunk4 psbTWinCreateSemaphore,      psbXSysCreateSemaphore
thunk3 psbTWinWaitSemaphore,        psbXSysWaitSemaphore
thunk2 psbTWinSignalSemaphore,      psbXSysSignalSemaphore
thunk3 psbTWinCreateCond,           psbXSysCreateCond
thunk3 psbTWinWaitCond,             psbXSysWaitCond
thunk1 psbTWinSignalCond,           psbXSysSignalCond
thunk1 psbTWinSignalCondAll,        psbXSysSignalCondAll
thunk3 psbTWinWrite,                psbXSysWrite
thunk3 psbTWinRead,                 psbXSysRead
thunk3 psbTWinOpen,                 psbXSysOpen
thunk1 psbTWinClose,                psbXSysClose
thunk0 psbTWinIsNeoMode,            psbXSysIsNeoMode
thunk1 psbTWinTlsGetAddr,           psbXTlsGetAddr
thunk1 psbTWinCxaFinalize,          psbXCxaFinalize
thunk0 psbTWinGetRuntimeVersion,    psbXGetRuntimeVersion
thunk0 psbTWinGetRuntimeName,       psbXGetRuntimeName
thunk6 psbTWinGnmSubmit,            psbXGnmSubmitCommandBuffers   ; 6 args
thunk0 psbTWinStackChkFail,         psbXStackChkFail

_TEXT ENDS
END
