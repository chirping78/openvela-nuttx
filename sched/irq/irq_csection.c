/****************************************************************************
 * sched/irq/irq_csection.c
 *
 *   Copyright (C) 2016 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>

#include <nuttx/init.h>
#include <nuttx/spinlock.h>
#include <nuttx/sched_note.h>
#include <arch/irq.h>

#include "sched/sched.h"
#include "irq/irq.h"

#if defined(CONFIG_SMP) || defined(CONFIG_SCHED_INSTRUMENTATION_CSECTION)

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifdef CONFIG_SMP
/* This is the spinlock that enforces critical sections when interrupts are
 * disabled.
 */

volatile spinlock_t g_cpu_irqlock SP_SECTION = SP_UNLOCKED;

/* Used to keep track of which CPU(s) hold the IRQ lock. */

volatile spinlock_t g_cpu_irqsetlock SP_SECTION;
volatile cpu_set_t g_cpu_irqset SP_SECTION;
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifdef CONFIG_SMP
/* Handles nested calls to enter_critical section from interrupt handlers */

static uint8_t g_cpu_nestcount[CONFIG_SMP_NCPUS];
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: irq_waitlock
 *
 * Description:
 *   Spin to get g_irq_waitlock, handling a known deadlock condition:
 *
 *   A deadlock may occur if enter_critical_section is called from an
 *   interrupt handler.  Suppose:
 *
 *   - CPUn is in a critical section and has the g_cpu_irqlock spinlock.
 *   - CPUm takes an interrupt and attempts to enter the critical section.
 *   - It spins waiting on g_cpu_irqlock with interrupts disabled.
 *   - CPUn calls up_cpu_pause() to pause operation on CPUm.  This will
 *     issue an inter-CPU interrupt to CPUm
 *   - But interrupts are disabled on CPUm so the up_cpu_pause() is never
 *     handled, causing the deadlock.
 *
 *   This same deadlock can occur in the normal tasking case:
 *
 *   - A task on CPUn enters a critical section and has the g_cpu_irqlock
 *     spinlock.
 *   - Another task on CPUm attempts to enter the critical section but has
 *     to wait, spinning to get g_cpu_irqlock with interrupts disabled.
 *   - The task on CPUn causes a new task to become ready-torun and the
 *     scheduler selects CPUm.  CPUm is requested to pause via a pause
 *     interrupt.
 *   - But the task on CPUm is also attempting to enter the critical
 *     section.  Since it is spinning with interrupts disabled, CPUm cannot
 *     process the pending pause interrupt, causing the deadlock.
 *
 *   This function detects this deadlock condition while spinning with \
 *   interrupts disabled.
 *
 * Input Parameters:
 *   cpu - The index of CPU that is trying to enter the critical section.
 *
 * Returned Value:
 *   True:  The g_cpu_irqlock spinlock has been taken.
 *   False: The g_cpu_irqlock spinlock has not been taken yet, but there is
 *          a pending pause interrupt request.
 *
 ****************************************************************************/

#ifdef CONFIG_SMP
static inline bool irq_waitlock(int cpu)
{
  /* Duplicate the spin_lock() logic from spinlock.c, but adding the check
   * for the deadlock condition.
   */

  while (spin_trylock(&g_cpu_irqlock) == SP_LOCKED)
    {
      /* Is a pause request pending? */

      if (up_cpu_pausereq(cpu))
        {
          /* Yes.. some other CPU is requesting to pause this CPU!
           * Abort the wait and return false.
           */

          return false;
        }

      SP_DSB();
    }

  /* We have g_cpu_irqlock! */

  SP_DMB();
  return true;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: enter_critical_section
 *
 * Description:
 *   Take the CPU IRQ lock and disable interrupts on all CPUs.  A thread-
 *   specific counter is increment to indicate that the thread has IRQs
 *   disabled and to support nested calls to enter_critical_section().
 *
 ****************************************************************************/

#ifdef CONFIG_SMP
irqstate_t enter_critical_section(void)
{
  FAR struct tcb_s *rtcb;
  irqstate_t ret;
  int cpu;

  /* Disable interrupts.
   *
   * NOTE 1: Ideally this should disable interrupts on all CPUs, but most
   * architectures only support disabling interrupts on the local CPU.
   * NOTE 2: Interrupts may already be disabled, but we call up_irq_save()
   * unconditionally because we need to return valid interrupt status in any
   * event.
   * NOTE 3: We disable local interrupts BEFORE taking the spinlock in order
   * to prevent possible waits on the spinlock from interrupt handling on
   * the local CPU.
   */

try_again:

  ret = up_irq_save();

  /* Verify that the system has sufficiently initialized so that the task
   * lists are valid.
   */

  if (g_os_initstate >= OSINIT_TASKLISTS)
    {
      /* If called from an interrupt handler, then just take the spinlock.
       * If we are already in a critical section, this will lock the CPU
       * in the interrupt handler.  Sounds worse than it is.
       */

      if (up_interrupt_context())
        {
          /* We are in an interrupt handler.  How can this happen?
           *
           *   1. We were not in a critical section when the interrupt
           *      occurred.  In this case, the interrupt was entered with:
           *
           *      g_cpu_irqlock = SP_UNLOCKED.
           *      g_cpu_nestcount = 0
           *      All CPU bits in g_cpu_irqset should be zero
           *
           *   2. We were in a critical section and interrupts on this
           *      this CPU were disabled -- this is an impossible case.
           *
           *   3. We were in critical section, but up_irq_save() only
           *      disabled local interrupts on a different CPU;
           *      Interrupts could still be enabled on this CPU.
           *
           *      g_cpu_irqlock = SP_LOCKED.
           *      g_cpu_nestcount = 0
           *      The bit in g_cpu_irqset for this CPU should be zero
           *
           *   4. An extension of 3 is that we may be re-entered numerous
           *      times from the same interrupt handler.  In that case:
           *
           *      g_cpu_irqlock = SP_LOCKED.
           *      g_cpu_nestcount > 0
           *      The bit in g_cpu_irqset for this CPU should be zero
           *
           * NOTE: However, the interrupt entry conditions can change due
           * to previous processing by the interrupt handler that may
           * instantiate a new thread that has irqcount > 0 and may then
           * set the bit in g_cpu_irqset and g_cpu_irqlock = SP_LOCKED
           */

          /* Handle nested calls to enter_critical_section() from the same
           * interrupt.
           */

          cpu = this_cpu();
          if (g_cpu_nestcount[cpu] > 0)
            {
              DEBUGASSERT(spin_islocked(&g_cpu_irqlock) &&
                          g_cpu_nestcount[cpu] < UINT8_MAX);
              g_cpu_nestcount[cpu]++;
            }

          /* This is the first call to enter_critical_section from the
           * interrupt handler.
           */

          else
            {
              /* Make sure that the g_cpu_irqlock() was not already set
               * by previous logic on this CPU that was executed by the
               * interrupt handler.  We know that the bit in g_cpu_irqset
               * for this CPU was zero on entry into the interrupt handler,
               * so if it is non-zero now then we know that was the case.
               */

              if ((g_cpu_irqset & (1 << cpu)) == 0)
                {
                  /* Wait until we can get the spinlock (meaning that we are
                   * no longer blocked by the critical section).
                   */

                  if (!irq_waitlock(cpu))
                    {
                      /* We are in a deadlock condition due to a pending
                       * pause request interrupt request.  Break the
                       * deadlock by handling the pause interrupt now.
                       */

                      DEBUGVERIFY(up_cpu_paused(cpu));
                    }
                }

              /* In any event, the nesting count is now one */

              g_cpu_nestcount[cpu] = 1;
            }
        }
      else
        {
          /* Normal tasking environment. */
          /* Do we already have interrupts disabled? */

           rtcb = this_task();
           DEBUGASSERT(rtcb != NULL);

           if (rtcb->irqcount > 0)
            {
              /* Yes... make sure that the spinlock is set and increment the
               * IRQ lock count.
               *
               * NOTE: If irqcount > 0 then (1) we are in a critical section, and
               * (2) this CPU should hold the lock.
               */

              DEBUGASSERT(spin_islocked(&g_cpu_irqlock) &&
                          (g_cpu_irqset & (1 << this_cpu())) != 0 &&
                          rtcb->irqcount < INT16_MAX);
              rtcb->irqcount++;
            }
          else
            {
              /* If we get here with irqcount == 0, then we know that the
               * current task running on this CPU is not in a critical
               * section.  However other tasks on other CPUs may be in a
               * critical section.  If so, we must wait until they release
               * the spinlock.
               */

              cpu = this_cpu();
              DEBUGASSERT((g_cpu_irqset & (1 << cpu)) == 0);

              if (!irq_waitlock(cpu))
                {
                  /* We are in a deadlock condition due to a pending pause
                   * request interrupt.  Re-enable interrupts on this CPU
                   * and try again.  Briefly re-enabling interrupts should
                   * be sufficient to permit processing the pending pause
                   * request.
                   */

                  up_irq_restore(ret);
                  goto try_again;
                }

              /* The set the lock count to 1.
               *
               * Interrupts disables must follow a stacked order.  We
               * cannot other context switches to re-order the enabling
               * disabling of interrupts.
               *
               * The scheduler accomplishes this by treating the irqcount
               * like lockcount:  Both will disable pre-emption.
               */

              spin_setbit(&g_cpu_irqset, cpu, &g_cpu_irqsetlock,
                          &g_cpu_irqlock);
              rtcb->irqcount = 1;

#ifdef CONFIG_SCHED_INSTRUMENTATION_CSECTION
              /* Note that we have entered the critical section */

              sched_note_csection(rtcb, true);
#endif
            }
        }
    }

  /* Return interrupt status */

  return ret;
}
#else /* defined(CONFIG_SCHED_INSTRUMENTATION_CSECTION) */
irqstate_t enter_critical_section(void)
{
  irqstate_t ret;

  /* Disable interrupts */

  ret = up_irq_save();

  /* Check if we were called from an interrupt handler and that the task
   * lists have been initialized.
   */

  if (!up_interrupt_context() && g_os_initstate >= OSINIT_TASKLISTS)
    {
      FAR struct tcb_s *rtcb = this_task();
      DEBUGASSERT(rtcb != NULL);

      /* Yes.. Note that we have entered the critical section */

      sched_note_csection(rtcb, true);
    }

  /* Return interrupt status */

  return ret;
}
#endif

/****************************************************************************
 * Name: leave_critical_section
 *
 * Description:
 *   Decrement the IRQ lock count and if it decrements to zero then release
 *   the spinlock.
 *
 ****************************************************************************/

#ifdef CONFIG_SMP
void leave_critical_section(irqstate_t flags)
{
  int cpu;

  /* Verify that the system has sufficiently initialized so that the task
   * lists are valid.
   */

  if (g_os_initstate >= OSINIT_TASKLISTS)
    {
      /* If called from an interrupt handler, then just release the
       * spinlock.  The interrupt handling logic should already hold the
       * spinlock if enter_critical_section() has been called.  Unlocking
       * the spinlock will allow interrupt handlers on other CPUs to execute
       * again.
       */

      if (up_interrupt_context())
        {
          /* We are in an interrupt handler. Check if the last call to
           * enter_critical_section() was nested.
           */

          cpu = this_cpu();
          if (g_cpu_nestcount[cpu] > 1)
            {
              /* Yes.. then just decrement the nesting count */

              DEBUGASSERT(spin_islocked(&g_cpu_irqlock));
              g_cpu_nestcount[cpu]--;
            }
          else
            {
              /* No, not nested. Release the spinlock. */

              DEBUGASSERT(spin_islocked(&g_cpu_irqlock) &&
                          g_cpu_nestcount[cpu] == 1);

              spin_lock(&g_cpu_irqsetlock); /* Protects g_cpu_irqset */
              if (g_cpu_irqset == 0)
                {
                  spin_unlock(&g_cpu_irqlock);
                }

              spin_unlock(&g_cpu_irqsetlock);
              g_cpu_nestcount[cpu] = 0;
            }
        }
      else
        {
          FAR struct tcb_s *rtcb = this_task();
          DEBUGASSERT(rtcb != NULL && rtcb->irqcount > 0);

          /* Normal tasking context.  We need to coordinate with other
           * tasks.
           *
           * Will we still have interrupts disabled after decrementing the
           * count?
           */

          if (rtcb->irqcount > 1)
            {
              /* Yes... the spinlock should remain set */

              DEBUGASSERT(spin_islocked(&g_cpu_irqlock));
              rtcb->irqcount--;
            }
          else
            {
 #ifdef CONFIG_SCHED_INSTRUMENTATION_CSECTION
              /* No.. Note that we have left the critical section */

              sched_note_csection(rtcb, false);
#endif
              /* Decrement our count on the lock.  If all CPUs have
               * released, then unlock the spinlock.
               */

              cpu = this_cpu();
              DEBUGASSERT(spin_islocked(&g_cpu_irqlock) &&
                          (g_cpu_irqset & (1 << cpu)) != 0);

              rtcb->irqcount = 0;
              spin_clrbit(&g_cpu_irqset, cpu, &g_cpu_irqsetlock,
                          &g_cpu_irqlock);

              /* Have all CPUs released the lock? */

              if (!spin_islocked(&g_cpu_irqlock))
                {
                  /* Check if there are pending tasks and that pre-emption
                   * is also enabled.
                   *
                   * REVISIT: Is there an issue here?  up_release_pending()
                   * must be called from within a critical section but here
                   * we have just left the critical section.  At least we
                   * still have interrupts disabled on this CPU.
                   */

                  if (g_pendingtasks.head != NULL &&
                      !spin_islocked(&g_cpu_schedlock))
                    {
                      /* Release any ready-to-run tasks that have collected
                       * in g_pendingtasks if the scheduler is not locked.
                       *
                       * NOTE: This operation has a very high likelihood of
                       * causing this task to be switched out!
                       *
                       * REVISIT: Should this not be done while we are in the
                       * critical section.
                       */

                      up_release_pending();
                    }
                }
            }
        }
    }

  /* Restore the previous interrupt state which may still be interrupts
   * disabled (but we don't have a mechanism to verify that now)
   */

  up_irq_restore(flags);
}
#else /* defined(CONFIG_SCHED_INSTRUMENTATION_CSECTION) */
void leave_critical_section(irqstate_t flags)
{
  /* Check if we were called from an interrupt handler and that the tasks
   * lists have been initialized.
   */

  if (!up_interrupt_context() && g_os_initstate >= OSINIT_TASKLISTS)
    {
      FAR struct tcb_s *rtcb = this_task();
      DEBUGASSERT(rtcb != NULL);

      /* Yes.. Note that we have left the critical section */

      sched_note_csection(rtcb, false);
    }

  /* Restore the previous interrupt state. */

  up_irq_restore(flags);
}
#endif

#endif /* CONFIG_SMP || CONFIG_SCHED_INSTRUMENTATION_CSECTION*/
