/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2012  MIPS Technologies, Inc.  All rights reserved.
 * Authors: Sanjay Lal <sanjayl@kymasys.com>
 *
 * Copyright (C) 2015 Imagination Technologies
 */

#include "hw/hw.h"
#include "hw/sysbus.h"
#include "qemu/bitmap.h"
#include "exec/memory.h"
#include "sysemu/sysemu.h"
#include "qom/cpu.h"
#include "exec/address-spaces.h"

#ifdef CONFIG_KVM
#include "sysemu/kvm.h"
#include "kvm_mips.h"
#endif

#include "hw/intc/mips_gic.h"

#define TIMER_PERIOD 10 /* 10 ns period for 100 Mhz frequency */

static inline int gic_get_current_vp(MIPSGICState *g)
{
    if (g->num_cpu > 1) {
        return current_cpu->cpu_index;
    }
    return 0;
}

static void gic_set_vp_irq(MIPSGICState *gic, int vpe, int pin, int level)
{
    int ored_level = level;
    int i;
    /* ORing pending registers sharing same pin */
    if (!ored_level) {
        for (i = 0; i < gic->num_irq; i++) {
            if ((gic->irq_state[i].map_pin & GIC_MAP_MSK) == pin &&
                    gic->irq_state[i].map_vpe == vpe &&
                    gic->irq_state[i].enabled) {
                ored_level |= gic->irq_state[i].pending;
            }
            if (ored_level) {
                /* no need to iterate all interrupts */
                break;
            }
        }
        if (((gic->vps[vpe].compare_map & GIC_MAP_MSK) == pin) &&
                (gic->vps[vpe].mask & GIC_VPE_MASK_CMP_MSK)) {
            /* ORing with local pending register (count/compare) */
            ored_level |= (gic->vps[vpe].pend & GIC_VPE_MASK_CMP_MSK) >>
                          GIC_VPE_MASK_CMP_SHF;
        }
    }

#ifdef CONFIG_KVM
    if (kvm_enabled())  {
        kvm_mips_set_ipi_interrupt(gic->vps[vpe].env, pin + GIC_CPU_PIN_OFFSET,
                                   ored_level);
    }
#endif
    qemu_set_irq(gic->vps[vpe].env->irq[pin + GIC_CPU_PIN_OFFSET], ored_level);
}

static void gic_set_irq(void *opaque, int n_IRQ, int level)
{
    MIPSGICState *gic = (MIPSGICState *) opaque;
    int vpe = gic->irq_state[n_IRQ].map_vpe;
    int pin = gic->irq_state[n_IRQ].map_pin & GIC_MAP_MSK;

    gic->irq_state[n_IRQ].pending = (bool) level;

    /* fixme: this slows UART down in Linux */
//    if (!gic->irq_state[n_IRQ].enabled) {
//        /* GIC interrupt source disabled */
//        return;
//    }

    if (vpe < 0 || vpe >= gic->num_cpu) {
        return;
    }

    gic_set_vp_irq(gic, vpe, pin, level);
}

/* GIC VPE Local Timer */
static uint32_t gic_vpe_timer_update(MIPSGICState *gic, uint32_t vp_index)
{
    uint64_t now, next;
    uint32_t wait;

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    wait = gic->vps[vp_index].comparelo - gic->sh_counterlo -
            (uint32_t)(now / TIMER_PERIOD);
    next = now + (uint64_t)wait * TIMER_PERIOD;

    qemu_log("GIC timer scheduled, now = %llx, next = %llx (wait = %u)\n",
             (long long) now, (long long) next, wait);

    timer_mod(gic->vps[vp_index].gic_timer->qtimer , next);
    return wait;
}

static void gic_vpe_timer_expire(MIPSGICState *gic, uint32_t vp_index)
{
    uint32_t pin;
    pin = (gic->vps[vp_index].compare_map & GIC_MAP_MSK);
    qemu_log("GIC timer expire => VPE[%d] irq %d\n", vp_index, pin);
    gic_vpe_timer_update(gic, vp_index);
    gic->vps[vp_index].pend |= (1 << 1);

    if (gic->vps[vp_index].pend &
            (gic->vps[vp_index].mask & GIC_VPE_MASK_CMP_MSK)) {
        if (gic->vps[vp_index].compare_map & 0x80000000) {
            /* it is safe to set the irq high regardless of other GIC IRQs */
            qemu_irq_raise(gic->vps[vp_index].env->irq
                           [pin + GIC_CPU_PIN_OFFSET]);
        } else {
            qemu_log("    disabled!\n");
        }
    } else {
        qemu_log("    masked off!\n");
    }
}

static uint32_t gic_get_sh_count(MIPSGICState *gic)
{
    int i;
    if (gic->sh_config & GIC_SH_CONFIG_COUNTSTOP_MSK) {
        return gic->sh_counterlo;
    } else {
        uint64_t now;
        now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        for (i = 0; i < gic->num_cpu; i++) {
            if (timer_pending(gic->vps[i].gic_timer->qtimer )
                && timer_expired(gic->vps[i].gic_timer->qtimer , now)) {
                /* The timer has already expired.  */
                gic_vpe_timer_expire(gic, i);
            }
        }
        return gic->sh_counterlo + (uint32_t)(now / TIMER_PERIOD);
    }
}

static void gic_store_sh_count(MIPSGICState *gic, uint64_t count)
{
    int i;

    if ((gic->sh_config & GIC_SH_CONFIG_COUNTSTOP_MSK) ||
            !gic->vps[0].gic_timer) {
        gic->sh_counterlo = count;
    } else {
        /* Store new count register */
        gic->sh_counterlo = count -
            (uint32_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / TIMER_PERIOD);
        /* Update timer timer */
        for (i = 0; i < gic->num_cpu; i++) {
            gic_vpe_timer_update(gic, i);
        }
    }
}

static void gic_store_vpe_compare(MIPSGICState *gic, uint32_t vp_index,
                                  uint64_t compare)
{
    gic->vps[vp_index].comparelo = (uint32_t) compare;
    gic_vpe_timer_update(gic, vp_index);

    gic->vps[vp_index].pend &= ~(1 << 1);
    if (gic->vps[vp_index].compare_map & GIC_MAP_TO_PIN_MSK) {
        uint32_t pin = (gic->vps[vp_index].compare_map & GIC_MAP_MSK);
        gic_set_vp_irq(gic, vp_index, pin, 0);
    }
}

static void gic_vpe_timer_cb(void *opaque)
{
    MIPSGICTimerState *gic_timer = opaque;
    gic_timer->gic->sh_counterlo++;
    gic_vpe_timer_expire(gic_timer->gic, gic_timer->vp_index);
    gic_timer->gic->sh_counterlo--;
}

static void gic_timer_start_count(MIPSGICState *gic)
{
    gic_store_sh_count(gic, gic->sh_counterlo);
}

static void gic_timer_stop_count(MIPSGICState *gic)
{
    int i;

    /* Store the current value */
    gic->sh_counterlo +=
        (uint32_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / TIMER_PERIOD);
    for (i = 0; i < gic->num_cpu; i++) {
        timer_del(gic->vps[i].gic_timer->qtimer );
    }
}

static void gic_timer_init(MIPSGICState *gic, uint32_t ncpus)
{
    int i;
    for (i = 0; i < ncpus; i++) {
        gic->vps[i].gic_timer = (void *) g_malloc0(sizeof(MIPSGICTimerState));
        gic->vps[i].gic_timer->gic = gic;
        gic->vps[i].gic_timer->vp_index = i;
        gic->vps[i].gic_timer->qtimer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                               &gic_vpe_timer_cb,
                                               gic->vps[i].gic_timer);
    }
    gic_store_sh_count(gic, gic->sh_counterlo);
}

/* GIC Read VPE Local/Other Registers */
static uint64_t gic_read_vpe(MIPSGICState *gic, uint32_t vp_index, hwaddr addr,
                             unsigned size)
{
    switch (addr) {
    case GIC_VPE_CTL_OFS:
        return gic->vps[vp_index].ctl;
    case GIC_VPE_PEND_OFS:
        gic_get_sh_count(gic);
        return gic->vps[vp_index].pend;
    case GIC_VPE_MASK_OFS:
        return gic->vps[vp_index].mask;
    case GIC_VPE_WD_MAP_OFS:
        return gic->vps[vp_index].wd_map;
    case GIC_VPE_COMPARE_MAP_OFS:
        return gic->vps[vp_index].compare_map;
    case GIC_VPE_TIMER_MAP_OFS:
        return gic->vps[vp_index].timer_map;
    case GIC_VPE_OTHER_ADDR_OFS:
        return gic->vps[vp_index].other_addr;
    case GIC_VPE_IDENT_OFS:
        return vp_index;
    case GIC_VPE_COMPARE_LO_OFS:
        return gic->vps[vp_index].comparelo;
    case GIC_VPE_COMPARE_HI_OFS:
        return gic->vps[vp_index].comparehi;
    default:
        qemu_log_mask(LOG_UNIMP,
                     "Warning *** read %d bytes at GIC offset LOCAL/OTHER 0x%"
                     PRIx64 "\n",
                     size, addr);
        break;
    }
    return 0;
}

static uint64_t gic_read(void *opaque, hwaddr addr, unsigned size)
{
    MIPSGICState *gic = (MIPSGICState *) opaque;
    uint32_t vp_index = gic_get_current_vp(gic);
    uint64_t ret = 0;
    int i, base, irq_src;
    uint32_t other_index;

    switch (addr) {
    case GIC_SH_CONFIG_OFS:
        ret =  gic->sh_config;
        break;
    case GIC_SH_COUNTERLO_OFS:
        ret = gic_get_sh_count(gic);
        break;
    case GIC_SH_COUNTERHI_OFS:
        ret = 0;
        break;
    case GIC_SH_POL_31_0_OFS ... GIC_SH_POL_255_224_OFS:
        base = (addr - GIC_SH_POL_31_0_OFS) * 8;
        for (i = 0; i < size * 8; i++) {
            ret |= (gic->irq_state[base + i].polarity & 1) << i;
        }
        break;
    case GIC_SH_TRIG_31_0_OFS ... GIC_SH_TRIG_255_224_OFS:
        base = (addr - GIC_SH_TRIG_31_0_OFS) * 8;
        for (i = 0; i < size * 8; i++) {
            ret |= (gic->irq_state[base + i].trigger_type & 1) << i;
        }
        break;
    case GIC_SH_PEND_31_0_OFS ... GIC_SH_PEND_255_224_OFS:
        base = (addr - GIC_SH_PEND_31_0_OFS) * 8;
        for (i = 0; i < size * 8; i++) {
            ret |= (gic->irq_state[base + i].pending & 1) << i;
        }
        break;
    case GIC_SH_MASK_31_0_OFS ... GIC_SH_MASK_255_224_OFS:
        base = (addr - GIC_SH_MASK_31_0_OFS) * 8;
        for (i = 0; i < size * 8; i++) {
            ret |= (gic->irq_state[base + i].enabled & 1) << i;
        }
        break;
    case GIC_SH_MAP0_PIN_OFS ... GIC_SH_MAP255_PIN_OFS:
        irq_src = (addr - GIC_SH_MAP0_PIN_OFS) / 4;
        ret = gic->irq_state[irq_src].map_pin;
        break;
    case GIC_SH_MAP0_VPE31_0_OFS ... GIC_SH_MAP255_VPE63_32_OFS:
        irq_src = (addr - GIC_SH_MAP0_VPE31_0_OFS) / 32;
        if ((gic->irq_state[irq_src].map_vpe) >= 0) {
            ret = 1 << (gic->irq_state[irq_src].map_vpe);
        } else {
            ret = 0;
        }
        break;
    /* VPE-Local Register */
    case GIC_VPELOCAL_BASE_ADDR ... (GIC_VPELOCAL_BASE_ADDR + GIC_VL_BRK_GROUP):
        ret = gic_read_vpe(gic, vp_index, addr - GIC_VPELOCAL_BASE_ADDR, size);
        break;
    /* VPE-Other Register */
    case GIC_VPEOTHER_BASE_ADDR ... (GIC_VPEOTHER_BASE_ADDR + GIC_VL_BRK_GROUP):
        other_index = gic->vps[vp_index].other_addr;
        ret = gic_read_vpe(gic, other_index, addr - GIC_VPEOTHER_BASE_ADDR,
                           size);
        break;
    /* User-Mode Visible section */
    case GIC_USERMODE_BASE_ADDR + GIC_USER_MODE_COUNTERLO:
        ret = gic_get_sh_count(gic);
        break;
    case GIC_USERMODE_BASE_ADDR + GIC_USER_MODE_COUNTERHI:
        ret = 0;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
            "Warning *** read %d bytes at GIC offset 0x%" PRIx64 "\n",
            size, addr);
        break;
    }
    return ret;
}

/* GIC Write VPE Local/Other Registers */
static void gic_write_vpe(MIPSGICState *gic, uint32_t vp_index, hwaddr addr,
                              uint64_t data, unsigned size)
{
    switch (addr) {
    case GIC_VPE_CTL_OFS:
        gic->vps[vp_index].ctl &= ~GIC_VPE_CTL_EIC_MODE_MSK;
        gic->vps[vp_index].ctl |= data & GIC_VPE_CTL_EIC_MODE_MSK;
        break;
    case GIC_VPE_RMASK_OFS:
        gic->vps[vp_index].mask &= ~(data & GIC_VPE_SET_RESET_MSK) &
                                   GIC_VPE_SET_RESET_MSK;
        break;
    case GIC_VPE_SMASK_OFS:
        gic->vps[vp_index].mask |= (data & GIC_VPE_SET_RESET_MSK);
        break;
    case GIC_VPE_WD_MAP_OFS:
        gic->vps[vp_index].wd_map = data & GIC_MAP_TO_PIN_REG_MSK;
        break;
    case GIC_VPE_COMPARE_MAP_OFS:
        gic->vps[vp_index].compare_map = data & GIC_MAP_TO_PIN_REG_MSK;
        break;
    case GIC_VPE_TIMER_MAP_OFS:
        gic->vps[vp_index].timer_map = data & GIC_MAP_TO_PIN_REG_MSK;
        break;
    case GIC_VPE_OTHER_ADDR_OFS:
        if (data < gic->num_cpu) {
            gic->vps[vp_index].other_addr = data;
        }
        break;
    case GIC_VPE_OTHER_ADDR_OFS + 4:
        /* do nothing */
        break;
    case GIC_VPE_COMPARE_LO_OFS:
        gic_store_vpe_compare(gic, vp_index, data);
        break;
    case GIC_VPE_COMPARE_HI_OFS:
        /* do nothing */
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                "Warning *** write %d bytes at GIC offset LOCAL/OTHER "
                "0x%" PRIx64" 0x%08lx\n", size, addr, data);
        break;
    }
}

static void gic_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    int intr;
    MIPSGICState *gic = (MIPSGICState *) opaque;
    uint32_t vp_index = gic_get_current_vp(gic);
    int i, base, irq_src;
    uint32_t pre, other_index;

    switch (addr) {
    case GIC_SH_CONFIG_OFS:
        pre = gic->sh_config;
        gic->sh_config = (gic->sh_config & ~GIC_SH_CONFIG_COUNTSTOP_MSK) |
                         (data & GIC_SH_CONFIG_COUNTSTOP_MSK);
        if (pre != gic->sh_config) {
            if ((gic->sh_config & GIC_SH_CONFIG_COUNTSTOP_MSK)) {
                gic_timer_stop_count(gic);
            } else {
                gic_timer_start_count(gic);
            }
        }
        break;
    case GIC_SH_COUNTERLO_OFS:
        if (gic->sh_config & GIC_SH_CONFIG_COUNTSTOP_MSK) {
            gic_store_sh_count(gic, data);
        }
        break;
    case GIC_SH_COUNTERHI_OFS:
        /* do nothing */
        break;
    case GIC_SH_POL_31_0_OFS ... GIC_SH_POL_255_224_OFS:
        base = (addr - GIC_SH_POL_31_0_OFS) * 8;
        for (i = 0; i < size * 8; i++) {
            gic->irq_state[base + i].polarity = (data >> i) & 1;
        }
        break;
    case GIC_SH_TRIG_31_0_OFS ... GIC_SH_TRIG_255_224_OFS:
        base = (addr - GIC_SH_TRIG_31_0_OFS) * 8;
        for (i = 0; i < size * 8; i++) {
            gic->irq_state[base + i].trigger_type = (data >> i) & 1;
        }
        break;
    case GIC_SH_RMASK_31_0_OFS ... GIC_SH_RMASK_255_224_OFS:
        base = (addr - GIC_SH_RMASK_31_0_OFS) * 8;
        for (i = 0; i < size * 8; i++) {
            gic->irq_state[base + i].enabled &= !((data >> i) & 1);
        }
        break;
    case GIC_SH_WEDGE_OFS:
        /* Figure out which VPE/HW Interrupt this maps to */
        intr = data & 0x7FFFFFFF;
        /* Mask/Enabled Checks */
        if (intr < gic->num_irq) {
            if (data & GIC_SH_WEDGE_RW_MSK) {
                gic_set_irq(gic, intr, 1);
            } else {
                gic_set_irq(gic, intr, 0);
            }
        }
        break;
    case GIC_SH_SMASK_31_0_OFS ... GIC_SH_SMASK_255_224_OFS:
        base = (addr - GIC_SH_SMASK_31_0_OFS) * 8;
        for (i = 0; i < size * 8; i++) {
            gic->irq_state[base + i].enabled |= (data >> i) & 1;
        }
        break;
    case GIC_SH_MAP0_PIN_OFS ... GIC_SH_MAP255_PIN_OFS:
        irq_src = (addr - GIC_SH_MAP0_PIN_OFS) / 4;
        gic->irq_state[irq_src].map_pin = data;
        break;
    case GIC_SH_MAP0_VPE31_0_OFS ... GIC_SH_MAP255_VPE63_32_OFS:
        irq_src = (addr - GIC_SH_MAP0_VPE31_0_OFS) / 32;
        gic->irq_state[irq_src].map_vpe = (data)? ctz64(data) : -1;
        break;
    case GIC_VPELOCAL_BASE_ADDR ... (GIC_VPELOCAL_BASE_ADDR + GIC_VL_BRK_GROUP):
        gic_write_vpe(gic, vp_index, addr - GIC_VPELOCAL_BASE_ADDR, data, size);
        break;
    case GIC_VPEOTHER_BASE_ADDR ... (GIC_VPEOTHER_BASE_ADDR + GIC_VL_BRK_GROUP):
        other_index = gic->vps[vp_index].other_addr;
        gic_write_vpe(gic, other_index, addr - GIC_VPEOTHER_BASE_ADDR,
                      data, size);
        break;
    case GIC_USERMODE_BASE_ADDR + GIC_USER_MODE_COUNTERLO:
    case GIC_USERMODE_BASE_ADDR + GIC_USER_MODE_COUNTERHI:
        /* do nothing. Read-only section */
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                "Warning *** write %d bytes at GIC offset 0x%" PRIx64
                " 0x%08lx\n",
                size, addr, data);
        break;
    }
}

static void gic_reset(void *opaque)
{
    int i;
    MIPSGICState *gic = (MIPSGICState *) opaque;
    int numintrs = (((gic->num_irq) / 8) - 1);

    numintrs =  (numintrs < 0)? 0: numintrs;

    gic->sh_config      = (1 << GIC_SH_CONFIG_COUNTSTOP_SHF) |
                          (numintrs << GIC_SH_CONFIG_NUMINTRS_SHF) |
                          gic->num_cpu;
    gic->sh_counterlo   = 0;

    for (i = 0; i < gic->num_cpu; i++) {
        gic->vps[i].ctl         = 0x0;
        gic->vps[i].pend        = 0x0;
        gic->vps[i].mask        = 0;
        gic->vps[i].wd_map      = GIC_MAP_TO_NMI_MSK;
        gic->vps[i].compare_map = GIC_MAP_TO_PIN_MSK;
        gic->vps[i].timer_map   = GIC_MAP_TO_PIN_MSK | 0x5;
        gic->vps[i].comparelo   = 0x0;
        gic->vps[i].comparehi   = 0x0;
        gic->vps[i].other_addr  = 0x0;
    }

    for (i = 0; i < gic->num_irq; i++) {
        gic->irq_state[i].enabled        = false;
        gic->irq_state[i].pending        = false;
        gic->irq_state[i].polarity       = false;
        gic->irq_state[i].trigger_type   = false;
        gic->irq_state[i].dual_edge      = false;
        gic->irq_state[i].map_pin        = GIC_MAP_TO_PIN_MSK;
        gic->irq_state[i].map_vpe        = -1;
    }
}

static const MemoryRegionOps gic_ops = {
    .read = gic_read,
    .write = gic_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .max_access_size = 8,
    },
};

static void mips_gic_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    MIPSGICState *s = MIPS_GIC(obj);

    memory_region_init_io(&s->gic_mem, OBJECT(s), &gic_ops, s,
                          "mips-gic", GIC_ADDRSPACE_SZ);
    sysbus_init_mmio(sbd, &s->gic_mem);
    qemu_register_reset(gic_reset, s);
}

static void mips_gic_realize(DeviceState *dev, Error **errp)
{
    MIPSGICState *s = MIPS_GIC(dev);
    CPUState *cs = first_cpu;
    int i;

    if (s->num_cpu > GIC_MAX_VPS) {
        error_setg(errp, "Exceed maximum CPUs %d", s->num_cpu);
        return;
    }
    if (s->num_irq > GIC_MAX_INTRS) {
        error_setg(errp, "Exceed maximum GIC IRQs %d", s->num_irq);
        return;
    }

    s->vps = g_new(MIPSGICVPState, s->num_cpu);
    s->irq_state = g_new(MIPSGICIRQState, s->num_irq);

    /* Register the env for all VPs with the GIC */
    for (i = 0; i < s->num_cpu; i++) {
        if (cs != NULL) {
            s->vps[i].env = cs->env_ptr;
            cs = CPU_NEXT(cs);
        } else {
            fprintf(stderr, "Unable to initialize GIC - CPUState for "
                    "CPU #%d not valid!", i);
            return;
        }
    }

    gic_timer_init(s, s->num_cpu);

    qdev_init_gpio_in(dev, gic_set_irq, s->num_irq);
    for (i = 0; i < s->num_irq; i++) {
        s->irq_state[i].irq = qdev_get_gpio_in(dev, i);
    }
}

static Property mips_gic_properties[] = {
    DEFINE_PROP_INT32("num-cpu", MIPSGICState, num_cpu, 1),
    DEFINE_PROP_INT32("num-irq", MIPSGICState, num_irq, 256),
    DEFINE_PROP_END_OF_LIST(),
};

static void mips_gic_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->props = mips_gic_properties;
    dc->realize = mips_gic_realize;
}

static const TypeInfo mips_gic_info = {
    .name          = TYPE_MIPS_GIC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MIPSGICState),
    .instance_init = mips_gic_init,
    .class_init    = mips_gic_class_init,
};

static void mips_gic_register_types(void)
{
    type_register_static(&mips_gic_info);
}

type_init(mips_gic_register_types)
