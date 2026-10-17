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
EXTERN psbXProbeCall1:PROC
EXTERN psbXProbeCall2:PROC
EXTERN psbXProbeCall3:PROC
EXTERN psbXProbeCall4:PROC
EXTERN psbXProbeCall5:PROC
EXTERN psbXProbeCall6:PROC
EXTERN psbXProbePtr2:PROC
EXTERN psbXSysFreeDirectMemory:PROC
EXTERN psbXSysMapNamedDirectMemory:PROC
EXTERN psbXSysMmap:PROC
EXTERN psbXSysMunmap:PROC
EXTERN psbXSysQueryMemoryProtection:PROC
EXTERN psbXSysCreateEqueue:PROC
EXTERN psbXSysDeleteEqueue:PROC
EXTERN psbXSysAddEvent:PROC
EXTERN psbXSysTriggerEvent:PROC
EXTERN psbXSysWaitEqueue:PROC
EXTERN psbXSysCancelEvent:PROC
EXTERN psbXSysLseek:PROC
EXTERN psbXSysFstat:PROC
EXTERN psbXGnmReserveGpuAddress:PROC
EXTERN psbXGnmMapGpuAddress:PROC
EXTERN psbXGnmUnmapGpuAddress:PROC
; --- Phase-4 surface (audio / pad / net / counters) ---------------------------
EXTERN psbXSysGetProcessTimeCounter:PROC
EXTERN psbXSysGetProcessTimeCounterFrequency:PROC
EXTERN psbXAudioOutOpen:PROC
EXTERN psbXAudioOutOutput:PROC
EXTERN psbXAudioOutSetVolume:PROC
EXTERN psbXAudioOutClose:PROC
EXTERN psbXPadOpen:PROC
EXTERN psbXPadReadState:PROC
EXTERN psbXPadRead:PROC
EXTERN psbXPadGetControllerInformation:PROC
EXTERN psbXNetInit:PROC
EXTERN psbXNetTerm:PROC
EXTERN psbXNetSocket:PROC
EXTERN psbXNetConnect:PROC
EXTERN psbXNetBind:PROC
EXTERN psbXNetListen:PROC
EXTERN psbXNetAccept:PROC
EXTERN psbXNetSend:PROC
EXTERN psbXNetRecv:PROC
EXTERN psbXNetCloseSocket:PROC
EXTERN psbXNetCtlInit:PROC
EXTERN psbXNetCtlTerm:PROC
EXTERN psbXNetCtlGetState:PROC

_TEXT SEGMENT

; ABI INVARIANT: every thunk assumes it is ENTERED BY CALL (guest `call` via
; the PLT), i.e. RSP % 16 == 8 on entry with the return address at [rsp].
; That makes RSP 16-aligned at each `call target`, as Win64 requires. Guests
; must not tail-jump imports (no `return sce*` wrappers in the corpus); if a
; tail-jumping guest ever appears, add dynamic alignment here.

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
; Phase-3 FIX (the Phase-2 residual-corruption root cause): r8 must be loaded
; from rdx BEFORE rdx is clobbered by the rsi move. The previous order
; (mov rdx, rsi; mov r8, rdx) delivered arg1 twice and LOST arg2 for every
; 3-argument guest->host call on Windows — sceKernelStartThread got a garbage
; argp, sceKernelWrite a garbage length, sceKernelCreateCond wrote its handle
; through a garbage out-pointer (stack scribble -> AV at exit).
thunk3 MACRO name, target
name PROC
    mov     rcx, rdi
    mov     r8,  rdx          ; arg2 read BEFORE rdx is clobbered
    mov     rdx, rsi
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

; --- arity 5: SysV a1..a5 ALL in registers (RDI,RSI,RDX,RCX,R8) ---------------
; SysV x86-64 passes args 1-6 in registers; the stack carries arg 7+. So a5
; arrives in R8 and must be captured BEFORE the shuffle clobbers it. Win64
; stack args start at caller [rsp+20h] (callee [rsp+28h] after ret push).
; Reserve 38h: 20h shadow + 8 a5 + 8 pad (entry RSP%16=8; 8+38h ≡ 0 at call).
thunk5 MACRO name, target
name PROC
    mov     rax, r8               ; SysV a5 (register, not stack!)
    mov     r9,  rcx              ; a4
    mov     r8,  rdx              ; a3
    mov     rdx, rsi              ; a2
    mov     rcx, rdi              ; a1
    sub     rsp, 38h
    mov     [rsp+20h], rax        ; Win64 a5 slot
    call    target
    add     rsp, 38h
    ret
name ENDP
ENDM

; --- arity 6: SysV a1..a6 ALL in registers (RDI,RSI,RDX,RCX,R8,R9) -------------
; a5 (R8) and a6 (R9) become Win64 stack args at [rsp+20h]/[rsp+28h].
; Both are captured BEFORE the shuffle clobbers R8/R9.
thunk6 MACRO name, target
name PROC
    mov     rax, r9               ; SysV a6
    mov     r10, r8               ; SysV a5
    mov     r9,  rcx              ; a4
    mov     r8,  rdx              ; a3
    mov     rdx, rsi              ; a2
    mov     rcx, rdi              ; a1
    sub     rsp, 38h              ; 20h shadow + 10h args + 8 pad, 16-aligned
    mov     [rsp+20h], r10        ; Win64 a5
    mov     [rsp+28h], rax        ; Win64 a6
    call    target
    add     rsp, 38h
    ret
name ENDP
ENDM

; --- arity 7: SysV a1..a6 in registers, a7 on the stack ([rsp+8] on entry) -----
; Win64: a1..a4 registers, a5/a6/a7 on the stack at [rsp+20h]/28h/30h.
; The old version wrote a7 at [rsp+38h] after sub 38h — which is the thunk's
; OWN return-address slot (entry [rsp]) — a guaranteed wild jump on first use.
; Reserve 48h: 20h shadow + 18h args + 10h pad (8+48h ≡ 0 at call).
thunk7 MACRO name, target
name PROC
    mov     rax, [rsp+8]          ; SysV a7 (correct: first SysV stack arg)
    mov     r10, r8               ; SysV a5
    mov     r11, r9               ; SysV a6
    mov     r9,  rcx              ; a4
    mov     r8,  rdx              ; a3
    mov     rdx, rsi              ; a2
    mov     rcx, rdi              ; a1
    sub     rsp, 48h
    mov     [rsp+20h], r10        ; Win64 a5
    mov     [rsp+28h], r11        ; Win64 a6
    mov     [rsp+30h], rax        ; Win64 a7
    call    target
    add     rsp, 48h
    ret
name ENDP
ENDM

; --- exports -----------------------------------------------------------------
thunk6 psbTWinAllocateDirectMemory, psbXSysAllocateDirectMemory   ; 6 args
thunk6 psbTWinMapDirectMemory,      psbXSysMapDirectMemory        ; 6 args
thunk1 psbTWinGetDirectMemorySize,  psbXSysGetDirectMemorySize
thunk0 psbTWinGetProcessTime,       psbXSysGetProcessTime
thunk0 psbTWinGetProcessTimeInTick, psbXSysGetProcessTimeInTick
thunk1 psbTWinUsleep,               psbXSysUsleep
thunk2 psbTWinNanosleep,            psbXSysNanosleep
thunk1 psbTWinSleep,                psbXSysSleep
thunk7 psbTWinCreateThread,         psbXSysCreateThread           ; 7 args (v1.2: +cpuAffinityMask/out)
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
thunk3 psbTWinLseek,                psbXSysLseek
thunk2 psbTWinFstat,                psbXSysFstat
thunk2 psbTWinFreeDirectMemory,     psbXSysFreeDirectMemory
thunk7 psbTWinMapNamedDirectMemory, psbXSysMapNamedDirectMemory  ; 7 args
thunk6 psbTWinMmap,                 psbXSysMmap
thunk2 psbTWinMunmap,               psbXSysMunmap
thunk4 psbTWinQueryMemoryProtection, psbXSysQueryMemoryProtection
thunk2 psbTWinCreateEqueue,         psbXSysCreateEqueue
thunk1 psbTWinDeleteEqueue,         psbXSysDeleteEqueue
thunk4 psbTWinAddEvent,             psbXSysAddEvent
thunk4 psbTWinTriggerEvent,         psbXSysTriggerEvent
thunk3 psbTWinWaitEqueue,           psbXSysWaitEqueue
thunk2 psbTWinCancelEvent,          psbXSysCancelEvent
thunk0 psbTWinIsNeoMode,            psbXSysIsNeoMode
thunk1 psbTWinTlsGetAddr,           psbXTlsGetAddr
thunk1 psbTWinCxaFinalize,          psbXCxaFinalize
thunk0 psbTWinGetRuntimeVersion,    psbXGetRuntimeVersion
thunk0 psbTWinGetRuntimeName,       psbXGetRuntimeName
thunk6 psbTWinGnmSubmit,            psbXGnmSubmitCommandBuffers   ; 6 args
thunk4 psbTWinGnmReserveGpuVa,      psbXGnmReserveGpuAddress      ; 4 args
thunk4 psbTWinGnmMapGpuVa,          psbXGnmMapGpuAddress          ; 4 args
thunk2 psbTWinGnmUnmapGpuVa,        psbXGnmUnmapGpuAddress        ; 2 args
thunk0 psbTWinStackChkFail,         psbXStackChkFail

; --- Phase-4 audio / pad / net / counter surface -------------------------------
; Arity audit (SysV arg counts):
;   GetProcessTimeCounter/Frequency  0    AudioOutOpen     6
;   AudioOutOutput                   2    AudioOutSetVolume 3
;   AudioOutClose                    1    PadOpen          4
;   PadReadState                     2    PadRead          4
;   PadGetControllerInformation      2    NetInit          1
;   NetTerm                          0    NetSocket        4
;   NetConnect/Bind                  3    NetListen        2
;   NetAccept                        3    NetSend/Recv     4
;   NetCloseSocket                   1    NetCtlInit/Term  0
;   NetCtlGetState                   1
thunk0 psbTWinGetProcessTimeCounter,       psbXSysGetProcessTimeCounter
thunk0 psbTWinGetProcessTimeCounterFrequency, psbXSysGetProcessTimeCounterFrequency
thunk6 psbTWinAudioOutOpen,                psbXAudioOutOpen              ; 6 args
thunk2 psbTWinAudioOutOutput,              psbXAudioOutOutput
thunk3 psbTWinAudioOutSetVolume,           psbXAudioOutSetVolume
thunk1 psbTWinAudioOutClose,               psbXAudioOutClose
thunk4 psbTWinPadOpen,                     psbXPadOpen
thunk2 psbTWinPadReadState,                psbXPadReadState
thunk4 psbTWinPadRead,                     psbXPadRead
thunk2 psbTWinPadGetControllerInformation, psbXPadGetControllerInformation
thunk1 psbTWinNetInit,                     psbXNetInit
thunk0 psbTWinNetTerm,                     psbXNetTerm
thunk4 psbTWinNetSocket,                   psbXNetSocket
thunk3 psbTWinNetConnect,                  psbXNetConnect
thunk3 psbTWinNetBind,                     psbXNetBind
thunk2 psbTWinNetListen,                   psbXNetListen
thunk3 psbTWinNetAccept,                   psbXNetAccept
thunk4 psbTWinNetSend,                     psbXNetSend
thunk4 psbTWinNetRecv,                     psbXNetRecv
thunk1 psbTWinNetCloseSocket,              psbXNetCloseSocket
thunk0 psbTWinNetCtlInit,                  psbXNetCtlInit
thunk0 psbTWinNetCtlTerm,                  psbXNetCtlTerm
thunk1 psbTWinNetCtlGetState,              psbXNetCtlGetState

; --- Phase-3 ABI conformance probes -------------------------------------------
; Deterministic argument-position checks exercised by the abi_probe guest so
; every thunk arity (register args AND stack args) is CI-verified on Windows.
thunk1 psbTWinProbeCall1,           psbXProbeCall1
thunk2 psbTWinProbeCall2,           psbXProbeCall2
thunk3 psbTWinProbeCall3,           psbXProbeCall3
thunk4 psbTWinProbeCall4,           psbXProbeCall4
thunk5 psbTWinProbeCall5,           psbXProbeCall5
thunk6 psbTWinProbeCall6,           psbXProbeCall6
thunk2 psbTWinProbePtr2,            psbXProbePtr2

; ---------------------------------------------------------------------------
; Host -> guest ABI bridge (the reverse direction of the thunks above).
; MSVC host code launching a guest worker thread has (arglen, argp, entry) in
; Win64 regs (RCX, RDX, R8); the GCC-compiled guest entry expects SysV
; (RDI = arglen, RSI = argp). Calling the guest directly from host code passes
; garbage arguments (the hello.exe ACCESS_VIOLATION: its worker dereferenced
; the garbage argp). The bridge shuffles and calls; the guest's int32 return
; arrives in EAX, which is the return register in both ABIs.
; ---------------------------------------------------------------------------
psbTWinGuestThreadEntry PROC
    mov     rdi, rcx              ; SysV arg1 = arglen
    mov     rsi, rdx              ; SysV arg2 = argp
    sub     rsp, 28h              ; shadow + 16-alignment (entry RSP%16 = 8)
    call    r8                    ; guest entry (runtime pointer)
    add     rsp, 28h
    ret
psbTWinGuestThreadEntry ENDP

_TEXT ENDS
END
