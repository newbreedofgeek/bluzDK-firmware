#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "spi_slave_stream.h"
#include "spi_slave.h"
#include "nrf_delay.h"

#include "debug.h"

void ble_radio_ntf_handler(bool radio_state)
{
	if(radio_state==true)
	{
		nrf_gpio_pin_set(SPIS_PTS_PIN);
	}
	else
	{
		nrf_gpio_pin_clear(SPIS_PTS_PIN);
	}
}

/**@brief Function to set the tx buffer for next transaction.
 *
 * This will fill the tx buffer with data that will be sent to the master on the next transaction.
 *
 * @param[in] data buffer
 * @param[in] data buffer
 */
void spi_slave_send_data(uint8_t *buf, uint16_t size)
{
	//let the Photon know we are about to transmit
	nrf_gpio_pin_set(SPIS_PTS_PIN);

	//just in case, wait for any previous transactions
	while (transmitting) { }
	transmitting = true;

	if (nrf_gpio_pin_read(SPIS_CSN_PIN) == 1 && !moreData) {
		//no transmission in progress, request the sempahore
		while (spi_slave_buffers_unset() != NRF_SUCCESS) { }
	}
	while (!spiStreamSemaphoreReleased) { }

	//in case there is more data that is being handed down, wait.
//	while (moreData) { }
//
//	//grab the semaphore, this way our data can't be clocked out until we are ready
//	DEBUG("Attempting to grab the semaphore");
//	while (spi_slave_buffers_unset() != NRF_SUCCESS) { }
//	while (!spiSlaveSemaphoreHeld) {  }
	busy = true;

	//now let's send the size of the data first
	m_tx_buf[0] = (( (size) & 0xFF00) >> 8);
	m_tx_buf[1] = ( (size) & 0xFF);
	nrf_gpio_pin_set(SPIS_SA_PIN);
	spi_slave_buffers_set(m_tx_buf, m_rx_buf, SPI_SLAVE_HW_TX_BUF_SIZE, SPI_SLAVE_HW_RX_BUF_SIZE);
	//and wait for that to finish
	busy = true;
	while (busy) { }

	//now lets send the data one chunk at a time
	for (int i = 0; i < size; i+=SPI_SLAVE_HW_TX_BUF_SIZE) {
		uint16_t chunkLength = (size-i > SPI_SLAVE_HW_TX_BUF_SIZE ? SPI_SLAVE_HW_TX_BUF_SIZE : size-i);
		memcpy(m_tx_buf, buf+i, chunkLength);
		//alert the particle board we have data to send
		nrf_gpio_pin_set(SPIS_SA_PIN);

		busy = true;
		while (busy) { }
	}
	transmitting = false;
	DEBUG("Done in spi_slave_send_data");
}

/**@brief Function for SPI slave event callback.
 *
 * This will get called upon receiving an SPI transaction complete event.
 *
 * @param[in] event SPI slave driver event.
 */
static void spi_slave_event_handle(spi_slave_evt_t event)
{
    uint32_t err_code;

    if (event.evt_type == SPI_SLAVE_XFER_DONE)
    {
    	if (busy) {
    		nrf_gpio_pin_clear(SPIS_SA_PIN);
    		nrf_gpio_pin_clear(SPIS_PTS_PIN);
			busy = false;
			spi_slave_buffers_set(m_tx_buf, m_rx_buf, SPI_SLAVE_HW_TX_BUF_SIZE, SPI_SLAVE_HW_RX_BUF_SIZE);
    	} else {
			if (event.rx_amount == 255) {
				memcpy(buf+currentRxBufferSize, m_rx_buf, 254);
				currentRxBufferSize+=254;
				moreData = true;
			} else {
				memcpy(buf+currentRxBufferSize, m_rx_buf, event.rx_amount);
				currentRxBufferSize += event.rx_amount;
				rx_callback(buf, currentRxBufferSize);
				currentRxBufferSize = 0;
				moreData = false;
			}
			//Set buffers.
			if (transmitting && !moreData) {
				while (spi_slave_buffers_unset() != NRF_SUCCESS) { }
			} else {
				err_code = spi_slave_buffers_set(m_tx_buf, m_rx_buf, SPI_SLAVE_HW_TX_BUF_SIZE, SPI_SLAVE_HW_RX_BUF_SIZE);
				APP_ERROR_CHECK(err_code);
			}
    	}
    } else if (event.evt_type == SPI_SLAVE_BUFFERS_SET_DONE) {
		spiStreamSemaphoreReleased = false;
    } else if (event.evt_type == SPI_SLAVE_RESOURCE_HELD) {
		spiStreamSemaphoreReleased = true;
    }
}

uint32_t spi_slave_stream_init(void (*a)(uint8_t *m_tx_buf, uint16_t size))
{
    uint32_t           err_code;
    spi_slave_config_t spi_slave_config;

    nrf_gpio_cfg_output(SPIS_SA_PIN);
    nrf_gpio_pin_clear(SPIS_SA_PIN);

    nrf_gpio_cfg_output(SPIS_PTS_PIN);
    nrf_gpio_pin_clear(SPIS_PTS_PIN);

	currentRxBufferSize = 0;
    // Enable Radio Notification, allows us to alert the master when we are busy
	err_code = ble_radio_notification_init(NRF_APP_PRIORITY_LOW,NRF_RADIO_NOTIFICATION_DISTANCE_800US,ble_radio_ntf_handler);
	APP_ERROR_CHECK(err_code);

    rx_callback = a;
	busy = false;
    transmitting = false;
	spiStreamSemaphoreReleased = false;
	moreData = false;

    err_code = spi_slave_evt_handler_register(spi_slave_event_handle);
    APP_ERROR_CHECK(err_code);

    spi_slave_config.pin_miso         = SPIS_MISO_PIN;
    spi_slave_config.pin_mosi         = SPIS_MOSI_PIN;
    spi_slave_config.pin_sck          = SPIS_SCK_PIN;
    spi_slave_config.pin_csn          = SPIS_CSN_PIN;
    spi_slave_config.mode             = SPI_MODE_0;
    spi_slave_config.bit_order        = SPIM_LSB_FIRST;
    spi_slave_config.def_tx_character = DEF_CHARACTER;
    spi_slave_config.orc_tx_character = ORC_CHARACTER;

    err_code = spi_slave_init(&spi_slave_config);
    APP_ERROR_CHECK(err_code);

    //Set buffers.
    err_code = spi_slave_buffers_set(m_tx_buf, m_rx_buf, sizeof(m_tx_buf), sizeof(m_rx_buf));
    APP_ERROR_CHECK(err_code);


    return NRF_SUCCESS;
}

bool spi_slave_ready()
{
	return transmitting == false && nrf_gpio_pin_read(SPIS_CSN_PIN) == 1;
}