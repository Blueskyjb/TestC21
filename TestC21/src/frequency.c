
#include <asf.h>
#include <string.h>
#include <frequency.h>
#include <UARTCommandConsole.h>

struct freqm_module freqm_instance;

static SemaphoreHandle_t freqSem = NULL;

static
void freqm_complete_callback(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(freqSem, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken)
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    return;
}

/************************    FREQM **********************/

/*
 * Use the built-in FREQM module to measure a clock frequency
 */

void configure_freqm(void)
{
    struct freqm_config config;
    freqm_get_config_defaults(&config);
    config.ref_clock_circles = 48;
    /* Using all the defaults for this case means that we will measure
     * GCLK0 using GCLK1 as a reference.
     * The measurement duration is config.ref_clock_circles: 127.
     * GCLK1 is 6 MHz.
     */
#ifdef EXTERNAL_CLOCK_TEST
    /* The input pin at SIG_GEN2 will be used as GCLK_IO4 */
    config.msr_clock_source = GCLK_GENERATOR_4;
#endif
   
    /* N.B.: If there's no signal on the input line, this call will hang
     * trying to synchronize the input clock channel.
     */
    freqm_init(&freqm_instance, FREQM, &config);
    configure_freqm_callbacks();

    if (freqSem == NULL) {
        freqSem = xSemaphoreCreateBinary();
        if (freqSem == NULL) 
            debug_msg("Semaphore not created!\r\n");
    }
}

void configure_freqm_callbacks(void)
{
    freqm_register_callback(&freqm_instance, freqm_complete_callback, 
        FREQM_CALLBACK_MEASURE_DONE);
    freqm_enable_callback(&freqm_instance, FREQM_CALLBACK_MEASURE_DONE);
}

void freqm_run(void)
{
    freqm_enable(&freqm_instance);
    freqm_start_measure(&freqm_instance);
}

int freqm_wait(uint32_t *result)
{
   int rval = xSemaphoreTake(freqSem, 10);

   if (rval == pdFALSE)
      return (-1);

   return (freqm_get_result_value(&freqm_instance, result));
}

/************************  FREQCOUNT **********************/

struct tc_module tc_instance;
uint32_t gFreq = 0;

volatile uint16_t cc0;
volatile uint16_t cc1;
volatile uint32_t my_func_counter = 0;

static void my_func(struct tc_module *instance)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    /* Take a snapshot of the timer's capture registers */
    cc0 = instance->hw->COUNT16.CC[0].reg;
    cc1 = instance->hw->COUNT16.CC[1].reg;
    my_func_counter++;
    if (my_func_counter == 3) {
        xSemaphoreGiveFromISR(freqSem, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken)
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

struct events_resource resource;
void evsys_configuration(void);

void freq_gpio_init(int pin)
{
    struct extint_chan_conf config;
    struct tc_config tc_config;
    struct tc_events events;
    struct extint_events ext_ev;
    int i;

    evsys_configuration();

    /* Set up channel 0 of TC0 for measuring pulse width.
     * This configuration will cause the timer reset to 0 when
     * it gets the first event, the next 2 events will be captured
     * as timestamps in the CC registers.  
     */
    memset(&events, 0, sizeof(events));
    events.event_action = TC_EVENT_ACTION_PWP;
    events.generate_event_on_compare_channel[0] = true;
    events.generate_event_on_compare_channel[1] = true;
    events.on_event_perform_action = true;
    events.invert_event_input = true;

    tc_get_config_defaults(&tc_config);
    /* 16 bit counter is OK.  For a reference clock of 1 MHz,
     * the lowest frequency we can measure is 1000000/65535 = 15.25 Hz
     */
    tc_config.counter_size = TC_COUNTER_SIZE_16BIT;
    tc_config.clock_source = GCLK_GENERATOR_2;
    tc_config.clock_prescaler = TC_CLOCK_PRESCALER_DIV2;
    tc_config.enable_capture_on_channel[0] = true;
    tc_config.enable_capture_on_channel[1] = true;

    tc_init(&tc_instance, TC0, &tc_config);
    tc_enable_events(&tc_instance, &events);
    events_attach_user(&resource, EVSYS_ID_USER_TC0_EVU);
    tc_register_callback(&tc_instance, my_func, TC_CALLBACK_CC_CHANNEL1);
#if 0  /* Jimmy test... don't enable callback until ready to read */
    tc_enable_callback(&tc_instance, TC_CALLBACK_CC_CHANNEL0);
#endif

    /* Semaphore to be used by callback */
    if (freqSem == NULL) {
        freqSem = xSemaphoreCreateBinary();
        if (freqSem == NULL) 
            debug_msg("Semaphore not created!\r\n");
    }

    /* Note: enable the timer but don't start it.  That occurs when the
     * timer module captures an event because we're using the PWP
     * mechanism.
     */
    tc_enable(&tc_instance);

    /* Set up the input pin (FREQ_COUNT on schematic) */
#if 1
    extint_chan_get_config_defaults(&config);
    config.gpio_pin = pin;
    config.gpio_pin_pull = EXTINT_PULL_NONE;
#if 0 /* High worked, but try rising */
    /* If this is used, PWP function will work correctly in the
     * TC module, i.e. it will give pulse width and period.
     */
    config.detection_criteria = EXTINT_DETECT_HIGH;
#else
    /* If this is used, the PWP function will give the period twice,
     * instead of pulse width and period.  This is actually OK -- we're
     * measuring square waves and one value is sufficient to characterize
     * the signal
     */
    config.detection_criteria = EXTINT_DETECT_RISING;
#endif

    /* Initialize events that can be generated by GPIO interrupt (EXTINT) 
     * This is a long winded way to initialize a bit vector of booleans,
     * but that's the API...
     */
    for (i = 0; i < 32; i++)
        ext_ev.generate_event_on_detect[i] = false;

    /* Enable the event we want, EXTINT[2].  Now this pin will generate an
     * event rather than an interrupt.
     */
    ext_ev.generate_event_on_detect[2] = true;
    extint_enable_events(&ext_ev);
    extint_chan_set_config(2, &config);
#endif
}

/*
 * Allocate an event channel.  The event source for that
 * channel will be the interrupt line 2 from EIC module.
 * (The EIC module is the multiplexer for GPIO interrupts.)
 */
void evsys_configuration(void)
{
    struct events_config config;
    events_get_config_defaults(&config);
    config.generator = EVSYS_ID_GEN_EIC_EXTINT_2;
#if 0
    config.edge_detect = EVENTS_EDGE_DETECT_BOTH;
#endif
    config.edge_detect = EVENTS_EDGE_DETECT_RISING;
    config.path = EVENTS_PATH_ASYNCHRONOUS;
    events_allocate(&resource, &config);
}

/*
 * Wait for a freqency measurement to complete and print the result.
 */
void printtc(void)
{
    uint32_t my_cc0;
    uint32_t my_cc1;
    int rval;

    /* Clear any pending interrupts and grab the interrupt counter */
    system_interrupt_enter_critical_section();
    my_cc0 = cc0;
    my_cc1 = cc1;
    my_func_counter = 0;
    tc_enable_callback(&tc_instance, TC_CALLBACK_CC_CHANNEL1);
    system_interrupt_leave_critical_section();

    rval = xSemaphoreTake(freqSem, 10);

    /* Get the data and disable the interrupt */
    system_interrupt_enter_critical_section();
    my_cc0 = cc0;
    my_cc1 = cc1;
    tc_disable_callback(&tc_instance, TC_CALLBACK_CC_CHANNEL1);
    system_interrupt_leave_critical_section();
    if (rval == pdFALSE) {
        debug_msg("frequency measurement timed out\r\n");
        return;
    }

    /* Print results */
    debug_msg("freq0 = ");
    printhex(1000000 / my_cc0, CRLF);
    debug_msg("freq1 = ");
    printhex(1000000 / my_cc1, CRLF);
}
