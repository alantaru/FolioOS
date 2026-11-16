# tools/patch_vm_uefi.ps1
$enc = New-Object System.Text.UTF8Encoding($false)
$path = "tools/vm_uefi64.c"

$content = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
$content = $content.Replace("`r`n", "`n")

$target_queues = @'
static SimMouseEvent g_mouse_queue[512];
static int g_mouse_q_head = 0;
static int g_mouse_q_tail = 0;

typedef struct {
    uint64_t x, y;
    uint32_t btn;
} SimAbsEvent;

static SimAbsEvent g_abs_queue[128];
static int g_abs_q_head = 0;
static int g_abs_q_tail = 0;

static EFI_INPUT_KEY g_key_queue[512];
static int g_key_q_head = 0;
static int g_key_q_tail = 0;

static void push_mouse_event(int32_t dx, int32_t dy, int32_t dz, uint8_t btn) {
    if (g_mouse_q_tail < 512) {
        g_mouse_queue[g_mouse_q_tail].dx = dx;
        g_mouse_queue[g_mouse_q_tail].dy = dy;
        g_mouse_queue[g_mouse_q_tail].dz = dz;
        g_mouse_queue[g_mouse_q_tail].left_btn = btn;
        g_mouse_q_tail++;
    }
}

static void push_abs_event(uint64_t x, uint64_t y, uint32_t btn) {
    if (g_abs_q_tail < 128) {
        g_abs_queue[g_abs_q_tail].x = x;
        g_abs_queue[g_abs_q_tail].y = y;
        g_abs_queue[g_abs_q_tail].btn = btn;
        g_abs_q_tail++;
    }
}

static void push_key_event(uint16_t scan, uint16_t ch) {
    if (g_key_q_tail < 512) {
        g_key_queue[g_key_q_tail].ScanCode = scan;
        g_key_queue[g_key_q_tail].UnicodeChar = ch;
        g_key_q_tail++;
    }
}
'@.Replace("`r`n", "`n")

$new_queues = @'
#define EVENT_Q_SIZE 1024

static SimMouseEvent g_mouse_queue[EVENT_Q_SIZE];
static int g_mouse_q_head = 0;
static int g_mouse_q_tail = 0;

typedef struct {
    uint64_t x, y;
    uint32_t btn;
} SimAbsEvent;

static SimAbsEvent g_abs_queue[EVENT_Q_SIZE];
static int g_abs_q_head = 0;
static int g_abs_q_tail = 0;

static EFI_INPUT_KEY g_key_queue[EVENT_Q_SIZE];
static int g_key_q_head = 0;
static int g_key_q_tail = 0;

static void push_mouse_event(int32_t dx, int32_t dy, int32_t dz, uint8_t btn) {
    int next = (g_mouse_q_tail + 1) % EVENT_Q_SIZE;
    if (next != g_mouse_q_head) {
        g_mouse_queue[g_mouse_q_tail].dx = dx;
        g_mouse_queue[g_mouse_q_tail].dy = dy;
        g_mouse_queue[g_mouse_q_tail].dz = dz;
        g_mouse_queue[g_mouse_q_tail].left_btn = btn;
        g_mouse_q_tail = next;
    }
}

static void push_abs_event(uint64_t x, uint64_t y, uint32_t btn) {
    int next = (g_abs_q_tail + 1) % EVENT_Q_SIZE;
    if (next != g_abs_q_head) {
        g_abs_queue[g_abs_q_tail].x = x;
        g_abs_queue[g_abs_q_tail].y = y;
        g_abs_queue[g_abs_q_tail].btn = btn;
        g_abs_q_tail = next;
    }
}

static void push_key_event(uint16_t scan, uint16_t ch) {
    int next = (g_key_q_tail + 1) % EVENT_Q_SIZE;
    if (next != g_key_q_head) {
        g_key_queue[g_key_q_tail].ScanCode = scan;
        g_key_queue[g_key_q_tail].UnicodeChar = ch;
        g_key_q_tail = next;
    }
}
'@.Replace("`r`n", "`n")

$target_active = @'
static EFI_STATUS __attribute__((ms_abi)) active_ptr_get_state(void *This, EFI_SIMPLE_POINTER_STATE *State) {
    (void)This;
    if (g_mouse_q_head >= g_mouse_q_tail) return EFI_NOT_READY;
    SimMouseEvent ev = g_mouse_queue[g_mouse_q_head++];
    State->RelativeMovementX = ev.dx;
    State->RelativeMovementY = ev.dy;
    State->RelativeMovementZ = ev.dz;
    State->LeftButton        = ev.left_btn;
    State->RightButton       = 0;
    return EFI_SUCCESS;
}
'@.Replace("`r`n", "`n")

$new_active = @'
static EFI_STATUS __attribute__((ms_abi)) active_ptr_get_state(void *This, EFI_SIMPLE_POINTER_STATE *State) {
    (void)This;
    if (g_mouse_q_head == g_mouse_q_tail) return EFI_NOT_READY;
    SimMouseEvent ev = g_mouse_queue[g_mouse_q_head];
    g_mouse_q_head = (g_mouse_q_head + 1) % EVENT_Q_SIZE;
    State->RelativeMovementX = ev.dx;
    State->RelativeMovementY = ev.dy;
    State->RelativeMovementZ = ev.dz;
    State->LeftButton        = ev.left_btn;
    State->RightButton       = 0;
    return EFI_SUCCESS;
}
'@.Replace("`r`n", "`n")

$target_abs = @'
static EFI_STATUS __attribute__((ms_abi)) abs_ptr_get_state(void *This, EFI_ABSOLUTE_POINTER_STATE *State) {
    (void)This;
    if (g_abs_q_head >= g_abs_q_tail) return EFI_NOT_READY;
    SimAbsEvent ev = g_abs_queue[g_abs_q_head++];
    State->CurrentX      = ev.x;
    State->CurrentY      = ev.y;
    State->CurrentZ      = 0;
    State->ActiveButtons = ev.btn;
    return EFI_SUCCESS;
}
'@.Replace("`r`n", "`n")

$new_abs = @'
static EFI_STATUS __attribute__((ms_abi)) abs_ptr_get_state(void *This, EFI_ABSOLUTE_POINTER_STATE *State) {
    (void)This;
    if (g_abs_q_head == g_abs_q_tail) return EFI_NOT_READY;
    SimAbsEvent ev = g_abs_queue[g_abs_q_head];
    g_abs_q_head = (g_abs_q_head + 1) % EVENT_Q_SIZE;
    State->CurrentX      = ev.x;
    State->CurrentY      = ev.y;
    State->CurrentZ      = 0;
    State->ActiveButtons = ev.btn;
    return EFI_SUCCESS;
}
'@.Replace("`r`n", "`n")

$target_key = @'
static EFI_STATUS __attribute__((ms_abi)) con_in_read_key(void *This, EFI_INPUT_KEY *Key) {
    (void)This;
    if (g_key_q_head >= g_key_q_tail) return EFI_NOT_READY;
    *Key = g_key_queue[g_key_q_head++];
    return EFI_SUCCESS;
}
'@.Replace("`r`n", "`n")

$new_key = @'
static EFI_STATUS __attribute__((ms_abi)) con_in_read_key(void *This, EFI_INPUT_KEY *Key) {
    (void)This;
    if (g_key_q_head == g_key_q_tail) return EFI_NOT_READY;
    *Key = g_key_queue[g_key_q_head];
    g_key_q_head = (g_key_q_head + 1) % EVENT_Q_SIZE;
    return EFI_SUCCESS;
}
'@.Replace("`r`n", "`n")

$target_wnd = @'
        case WM_MOUSEMOVE: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            uint8_t btn = (wParam & MK_LBUTTON) ? 1 : 0;
            push_abs_event((uint64_t)x, (uint64_t)y, btn);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            push_abs_event((uint64_t)x, (uint64_t)y, 1);
            return 0;
        }
        case WM_LBUTTONUP: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            push_abs_event((uint64_t)x, (uint64_t)y, 0);
            return 0;
        }
'@.Replace("`r`n", "`n")

$new_wnd = @'
        case WM_MOUSEMOVE: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            uint8_t btn = (wParam & MK_LBUTTON) ? 1 : 0;
            push_abs_event((uint64_t)x, (uint64_t)y, btn);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            push_abs_event((uint64_t)x, (uint64_t)y, 1);
            push_mouse_event(0, 0, 0, 1);
            return 0;
        }
        case WM_LBUTTONUP: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            push_abs_event((uint64_t)x, (uint64_t)y, 0);
            push_mouse_event(0, 0, 0, 0);
            return 0;
        }
'@.Replace("`r`n", "`n")

$content = $content.Replace($target_queues, $new_queues)
$content = $content.Replace($target_active, $new_active)
$content = $content.Replace($target_abs, $new_abs)
$content = $content.Replace($target_key, $new_key)
$content = $content.Replace($target_wnd, $new_wnd)

[System.IO.File]::WriteAllText($path, $content, $enc)
Write-Host "[OK] tools/vm_uefi64.c substituido com sucesso!" -ForegroundColor Green
