#include "asmfunc.h"
#include "console.hpp"
#include "font.hpp"
#include "frame_buffer_config.hpp"
#include "graphics.hpp"
#include "interrupt.hpp"
#include "logger.hpp"
#include "memory_manager.hpp"
#include "memory_map.hpp"
#include "mouse.hpp"
#include "newlib_support.c"
#include "paging.hpp"
#include "pci.hpp"
#include "queue.hpp"
#include "segment.hpp"
#include "usb/classdriver/mouse.hpp"
#include "usb/device.hpp"
#include "usb/memory.hpp"
#include "usb/xhci/trb.hpp"
#include "usb/xhci/xhci.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <vector>

const PixelColor kDesktopBGColor{45, 118, 237};
const PixelColor kDesktopMenuBarColor{119, 197, 255};

// pixel writer
char pixel_writer_buf[sizeof(RGBResv8BitPerColorPixelWriter)];
PixelWriter *pixel_writer;

// void *operator new(size_t size, void *buf) { return buf; }
void operator delete(void *obj) noexcept {}

// console
char console_buf[sizeof(Console)];
Console *console;

int printk(const char *format, ...) {
    va_list ap;
    int result;
    char s[1024];

    va_start(ap, format);
    result = vsprintf(s, format, ap);
    va_end(ap);

    console->PutString(s);
    return result;
}

char memory_manager_buf[sizeof(BitmapMemoryManager)];
BitmapMemoryManager *memory_manager;

char mouse_cursor_buf[sizeof(MouseCursor)];
MouseCursor *mouse_cursor;

void MouseObserver(int8_t displacement_x, int8_t displacement_y) {
    mouse_cursor->MoveRelative({displacement_x, displacement_y});
}

void SwitchEhci2Xhci(const pci::Device &xhc_dev) {
    bool intel_ehc_exist = false;
    for (int i = 0; i < pci::num_device; ++i) {
        if (pci::devices[i].class_code.Match(0x0cu, 0x03u, 0x20u) &&
            0x8086 == pci::ReadVenderId(pci::devices[i])) {
            intel_ehc_exist = true;
            break;
        }
    }
    if (!intel_ehc_exist) {
        return;
    }
    uint32_t superspeed_ports = pci::ReadConfReg(xhc_dev, 0xdc);
    pci::WriteConfReg(xhc_dev, 0xd8, superspeed_ports);
    uint32_t ehci2xhci_ports = pci::ReadConfReg(xhc_dev, 0xd4);
    pci::WriteConfReg(xhc_dev, 0xd0, ehci2xhci_ports);
    Log(kDebug, "SwitchEhci2Xhci: SS = %02, xHCI = %02x\n", superspeed_ports,
        ehci2xhci_ports);
}

usb::xhci::Controller *xhc;

struct Message {
    enum Type {
        kInterruptXHCI,
    } type;
};

ArrayQueue<Message> *main_queue;

// interrupt
__attribute__((interrupt)) void IntHandlerXHCI(InterruptFrame *frame) {
    main_queue->Push(Message{Message::kInterruptXHCI});
    NotifyEndOfInterrupt();
}

alignas(16) uint8_t kernel_main_stack[1024 * 1024];

// #@@range_begin(call_write_pixel)
extern "C" void
KernelMainNewStack(const FrameBufferConfig &frame_buffer_config_ref,
                   const MemoryMap &memory_map_ref) {
    FrameBufferConfig frame_buffer_config{frame_buffer_config_ref};
    MemoryMap memory_map{memory_map_ref};
    // setting display color
    switch (frame_buffer_config.pixel_format) {
    case kPixelRGBResv8BitPerColor:
        pixel_writer = new (pixel_writer_buf)
            RGBResv8BitPerColorPixelWriter{frame_buffer_config};
        break;
    case kPixelBGRResv8BitPerColor:
        pixel_writer = new (pixel_writer_buf)
            BGRResv8BitPerColorPixelWriter{frame_buffer_config};
        break;
    default:
        break;
    }
    // black screen
    FillReactangle(*pixel_writer, {0, 0},
                   {(int)frame_buffer_config.horizontal_resolution,
                    (int)frame_buffer_config.vertical_resolution},
                   {0, 0, 0});
    console = new (console_buf)
        Console(*pixel_writer, {255, 255, 255}, kDesktopBGColor);
    SetLogLevel(kInfo);
    FillReactangle(*pixel_writer, {0, 0},
                   {(int)frame_buffer_config.horizontal_resolution,
                    (int)frame_buffer_config.vertical_resolution},
                   kDesktopBGColor);
    FillReactangle(*pixel_writer, {0, 0},
                   {(int)frame_buffer_config.horizontal_resolution, 20},
                   kDesktopMenuBarColor);

    // Write A
    int i = 0;
    for (char c = '!'; c <= '~'; ++c, ++i) {
        WriteAscii(*pixel_writer, 8 * i, 50, c, {0, 0, 0});
    }
    WriteString(*pixel_writer, 0, 66, "Hello world!!", {0, 0, 255});

    FillReactangle(*pixel_writer, {50, 50}, {10, 10}, {255, 0, 255});
    DrawReactangle(*pixel_writer, {150, 150}, {150, 150}, {255, 0, 255});
    DrawCircle(*pixel_writer, {250, 250}, 100, {34, 0, 255});
    mouse_cursor = new (mouse_cursor_buf)
        MouseCursor{pixel_writer, {255, 0, 255}, {300, 200}};

    // set up  segment
    SetupSegments();
    const uint16_t kernel_cs = 1 << 3;
    const uint16_t kernel_ss = 2 << 3;
    SetDSAll(0);
    SetCSSS(kernel_cs, kernel_ss);

    // setup page table
    SetupIdentityPageTable();

    // setup memory manager
    ::memory_manager = new (memory_manager_buf) BitmapMemoryManager;

    // show avaliable memoery
    printk("memory map: %p\n", &memory_map);
    const auto memory_map_base = reinterpret_cast<uintptr_t>(memory_map.buffer);
    uintptr_t available_end = 0;
    for (uintptr_t iter = memory_map_base;
         iter < memory_map_base + memory_map.map_size;
         iter += memory_map.descriptor_size) {
        auto desc = reinterpret_cast<const MemoryDescriptor *>(iter);
        if (available_end < desc->physical_start) {
            memory_manager->MarkAllocated(
                FrameID{available_end / kBytesPerFrame},
                (desc->physical_start - available_end) / kBytesPerFrame);
        }
        const auto physical_end =
            desc->physical_start + desc->number_of_pages * kUEFIPageSize;
        if (IsAvailable(static_cast<MemoryType>(desc->type))) {
            available_end = physical_end;
        } else {
            memory_manager->MarkAllocated(
                FrameID{desc->physical_start / kBytesPerFrame},
                desc->number_of_pages * kUEFIPageSize / kBytesPerFrame);
        }
    }

    memory_manager->SetMemoryRange(FrameID{1},
                                   FrameID{available_end / kBytesPerFrame});

    // init interupt queue
    std::array<Message, 32> main_queue_data;
    ArrayQueue<Message> main_queue{main_queue_data};
    ::main_queue = &main_queue;

    auto err = pci::ScanAllBus();
    printk("ScanAllBus: %s\n", err.Name());

    for (int i = 0; i < pci::num_device; ++i) {
        const auto &dev = pci::devices[i];
        auto vender_id = pci::ReadVenderId(dev);
        auto class_code = pci::ReadClassCode(dev.bus, dev.device, dev.function);
        printk("%d.%d.%d: vend %04x, class %08x, head %02x\n", dev.bus,
               dev.device, dev.function, vender_id, class_code,
               dev.header_type);
    }

    pci::Device *xhc_dev = nullptr;
    for (int i = 0; i < pci::num_device; ++i) {
        if (pci::devices[i].class_code.Match(0x0cu, 0x03u, 0x30u)) {
            xhc_dev = &pci::devices[i];

            if (0x8086 == pci::ReadVenderId(*xhc_dev)) {
                break;
            }
        }
    }

    if (xhc_dev) {
        Log(kInfo, "xHC has been found: %d.%d.%d\n", xhc_dev->bus,
            xhc_dev->device, xhc_dev->function);
    }

    // Load idt
    SetIDTEntry(idt[InterruptVector::kXHCI],
                MakeIDTAttr(DescriptorType::kInterruptGate, 0),
                reinterpret_cast<uint64_t>(IntHandlerXHCI), kernel_cs);
    LoadIDT(sizeof(idt) - 1, reinterpret_cast<uintptr_t>(&idt[0]));

    // Configure MSI
    const uint8_t bsp_local_apic_id =
        *reinterpret_cast<const uint32_t *>(0xfee00020) >> 24;
    pci::ConfigureMSIFixedDestination(
        *xhc_dev, bsp_local_apic_id, pci::MSITriggerMode::kLevel,
        pci::MSIDeliveryMode::kFixed, InterruptVector::kXHCI, 0);

    const WithError<uint64_t> xhc_bar = pci::ReadBar(*xhc_dev, 0);
    Log(kDebug, "ReadBar: %s\n", xhc_bar.error.Name());
    const uint64_t xhc_mmio_base = xhc_bar.value & ~static_cast<uint64_t>(0xf);
    Log(kDebug, "xHC mmio_base = %08lx\n", xhc_mmio_base);

    usb::xhci::Controller xhc{xhc_mmio_base};

    if (0x8086 == pci::ReadVenderId(*xhc_dev)) {
        SwitchEhci2Xhci(*xhc_dev);
    }
    {
        auto err = xhc.Initialize();
        Log(kDebug, "xhc.Initialize: %s \n", err.Name());
    }

    Log(kInfo, "xHC starting\n");
    xhc.Run();

    ::xhc = &xhc;
    __asm__("sti");

    usb::HIDMouseDriver::default_observer = MouseObserver;

    for (int i = 1; i <= xhc.MaxPorts(); ++i) {
        auto port = xhc.PortAt(i);
        Log(kDebug, "Port %d, IsConnected=%d,\n", i, port.IsConnected());

        if (port.IsConnected()) {
            if (auto err = ConfigurePort(xhc, port)) {
                Log(kError, "failed to configure port: %s at %s:%d\n",
                    err.Name(), err.File(), err.Line());
                continue;
            }
        }
    }

    while (true) {
        __asm__("cli");
        if (main_queue.Count() == 0) {
            __asm__("sti\n\thlt");
            continue;
        }

        Message msg = main_queue.Front();
        main_queue.Pop();
        __asm__("sti");

        switch (msg.type) {
        case Message::kInterruptXHCI:
            while (xhc.PrimaryEventRing()->HasFront()) {
                if (auto err = ProcessEvent(xhc)) {
                    Log(kError, "Error while ProcessEvent: %s at %s:%d\n",
                        err.Name(), err.File(), err.Line());
                }
            }
            break;
        default:
            Log(kError, "Unknown message type: %d\n", msg.type);
        }
    }
}

extern "C" void __cxa_pure_virtual() {
    while (1) {
        __asm__("hlt");
    }
}
// #@@range_end(call_write_pixel)
