/**-----------------------------------------------------------------------------

 @file    madt.c
 @brief   Implementation of ACPI MADT (Multiple APIC Description Table)
          functions
 @details
 @verbatim

  The MADT describes all of the interrupt controllers in the system. It
  can be used to enumerate the processors currently available.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <sys/madt.h>
#include <sys/panic.h>
#include <sys/smp.h>
#include <lib/klog.h>

static madt_t* madt;

static uint64_t num_lapic = 0;
static madt_record_lapic_t* lapics[CPU_MAX];

static uint64_t num_ioapic = 0;
static madt_record_ioapic_t* io_apics[4];

uint32_t madt_get_num_ioapic() { return num_ioapic; }
uint32_t madt_get_num_lapic() { return num_lapic; }

madt_record_ioapic_t** madt_get_ioapics() { return io_apics; }
madt_record_lapic_t** madt_get_lapics() { return lapics; }

uint64_t madt_get_lapic_base() { return madt->lapic_addr; }

void madt_init()
{
    madt = (madt_t*)acpi_get_sdt("APIC");

    if (!madt)
        kpanic("MADT(APIC) not found\n");

    uint64_t size = madt->hdr.length - sizeof(madt_t);
    for (uint64_t i = 0; i < size;) {
        madt_record_hdr_t* rec = (madt_record_hdr_t*)(madt->records + i);
        switch (rec->type) {
        case MADT_RECORD_TYPE_LAPIC: {
            /* we support only 256 cpu's */
            if (num_lapic >= CPU_MAX)
                break;
            madt_record_lapic_t* lapic = (madt_record_lapic_t*)rec;
            lapics[num_lapic++] = lapic;
        } break;
        case MADT_RECORD_TYPE_IOAPIC: {
            /* we support only 2 ioapic's */
            if (num_ioapic > 2)
                break;
            madt_record_ioapic_t* ioapic = (madt_record_ioapic_t*)rec;
            io_apics[num_ioapic++] = ioapic;
        } break;
            /* TODO: Handle MADT_RECORD_TYPE_ISO and MADT_RECORD_TYPE_NMI */
        }
        i += rec->len;
    }

    klogi("MADT initialization finished\n");
}
