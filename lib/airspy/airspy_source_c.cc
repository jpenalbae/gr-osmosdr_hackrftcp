/* -*- c++ -*- */
/*
 * Copyright 2013 Dimitri Stolnikov <horiz0n@gmx.net>
 *
 * GNU Radio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * GNU Radio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * config.h is generated by configure.  It contains the results
 * of probing for features, options etc.  It should be the first
 * file included in your .cc file.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdexcept>
#include <iostream>

#include <boost/assign.hpp>
#include <boost/format.hpp>
#include <boost/detail/endian.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/thread/thread.hpp>

#include <gnuradio/io_signature.h>

#include "airspy_source_c.h"

#include "arg_helpers.h"

using namespace boost::assign;

#define AIRSPY_THROW_ON_ERROR(ret, msg) \
  if ( ret != AIRSPY_SUCCESS )  \
  throw std::runtime_error( boost::str( boost::format(msg " (%d) %s") \
      % ret % airspy_error_name((enum airspy_error)ret) ) );

#define AIRSPY_FUNC_STR(func, arg) \
  boost::str(boost::format(func "(%d)") % arg) + " has failed"

int airspy_source_c::_usage = 0;
boost::mutex airspy_source_c::_usage_mutex;

airspy_source_c_sptr make_airspy_source_c (const std::string & args)
{
  return gnuradio::get_initial_sptr(new airspy_source_c (args));
}

/*
 * Specify constraints on number of input and output streams.
 * This info is used to construct the input and output signatures
 * (2nd & 3rd args to gr::block's constructor).  The input and
 * output signatures are used by the runtime system to
 * check that a valid number and type of inputs and outputs
 * are connected to this block.  In this case, we accept
 * only 0 input and 1 output.
 */
static const int MIN_IN = 0;	// mininum number of input streams
static const int MAX_IN = 0;	// maximum number of input streams
static const int MIN_OUT = 1;	// minimum number of output streams
static const int MAX_OUT = 1;	// maximum number of output streams

/*
 * The private constructor
 */
airspy_source_c::airspy_source_c (const std::string &args)
  : gr::sync_block ("airspy_source_c",
        gr::io_signature::make(MIN_IN, MAX_IN, sizeof (gr_complex)),
        gr::io_signature::make(MIN_OUT, MAX_OUT, sizeof (gr_complex))),
    _dev(NULL),
    _sample_rate(0),
    _center_freq(0),
    _freq_corr(0),
    _auto_gain(false),
    _lna_gain(0),
    _mix_gain(0),
    _vga_gain(0),
    _bandwidth(0)
{
  int ret;

  dict_t dict = params_to_dict(args);

  {
    boost::mutex::scoped_lock lock( _usage_mutex );

    if ( _usage == 0 )
      airspy_init(); /* call only once before the first open */

    _usage++;
  }

  _dev = NULL;
  ret = airspy_open( &_dev );
  AIRSPY_THROW_ON_ERROR(ret, "Failed to open AirSpy device")

  uint8_t board_id;
  ret = airspy_board_id_read( _dev, &board_id );
  AIRSPY_THROW_ON_ERROR(ret, "Failed to get AirSpy board id")

  char version[40];
  memset(version, 0, sizeof(version));
  ret = airspy_version_string_read( _dev, version, sizeof(version));
  AIRSPY_THROW_ON_ERROR(ret, "Failed to read version string")
#if 0
  read_partid_serialno_t serial_number;
  ret = airspy_board_partid_serialno_read( _dev, &serial_number );
  AIRSPY_THROW_ON_ERROR(ret, "Failed to read serial number")
#endif
  std::cerr << "Using " << airspy_board_id_name(airspy_board_id(board_id)) << " "
            << "with firmware " << version << " "
            << std::endl;

  set_center_freq( (get_freq_range().start() + get_freq_range().stop()) / 2.0 );
  set_sample_rate( get_sample_rates().start() );
  set_bandwidth( 0 );

  set_gain( 8 ); /* preset to a reasonable default (non-GRC use case) */

  set_mix_gain( 5 ); /* preset to a reasonable default (non-GRC use case) */

  set_if_gain( 0 ); /* preset to a reasonable default (non-GRC use case) */

  _fifo = new boost::circular_buffer<gr_complex>(5000000);
  if (!_fifo) {
    throw std::runtime_error( std::string(__FUNCTION__) + " " +
                              "Failed to allocate a sample FIFO!" );
  }
}

/*
 * Our virtual destructor.
 */
airspy_source_c::~airspy_source_c ()
{
  int ret;

  if (_dev) {
    if ( airspy_is_streaming( _dev ) == AIRSPY_TRUE )
    {
      ret = airspy_stop_rx( _dev );
      AIRSPY_THROW_ON_ERROR(ret, "Failed to stop RX streaming")
    }

    ret = airspy_close( _dev );
    AIRSPY_THROW_ON_ERROR(ret, "Failed to close AirSpy")
    _dev = NULL;

    {
      boost::mutex::scoped_lock lock( _usage_mutex );

       _usage--;

      if ( _usage == 0 )
        airspy_exit(); /* call only once after last close */
    }
  }

  if (_fifo)
  {
    delete _fifo;
    _fifo = NULL;
  }
}

int airspy_source_c::_airspy_rx_callback(airspy_transfer *transfer)
{
  airspy_source_c *obj = (airspy_source_c *)transfer->ctx;

  return obj->airspy_rx_callback((float *)transfer->samples, transfer->sample_count);
}

int airspy_source_c::airspy_rx_callback(void *samples, int sample_count)
{
  size_t i, n_avail, to_copy, num_samples = sample_count;
  float *sample = (float *)samples;

  _fifo_lock.lock();

  n_avail = _fifo->capacity() - _fifo->size();
  to_copy = (n_avail < num_samples ? n_avail : num_samples);

  for (i = 0; i < to_copy; i++ )
  {
    /* Push sample to the fifo */
    _fifo->push_back( gr_complex( *sample, *(sample+1) ) );

    /* offset to the next I+Q sample */
    sample += 2;
  }

  _fifo_lock.unlock();

  /* We have made some new samples available to the consumer in work() */
  if (to_copy) {
    //std::cerr << "+" << std::flush;
    _samp_avail.notify_one();
  }

  /* Indicate overrun, if neccesary */
  if (to_copy < num_samples)
    std::cerr << "O" << std::flush;

  return 0; // TODO: return -1 on error/stop
}

bool airspy_source_c::start()
{
  if ( ! _dev )
    return false;

  int ret = airspy_start_rx( _dev, _airspy_rx_callback, (void *)this );
  if ( ret != AIRSPY_SUCCESS ) {
    std::cerr << "Failed to start RX streaming (" << ret << ")" << std::endl;
    return false;
  }

  return true;
}

bool airspy_source_c::stop()
{
  if ( ! _dev )
    return false;

  int ret = airspy_stop_rx( _dev );
  if ( ret != AIRSPY_SUCCESS ) {
    std::cerr << "Failed to stop RX streaming (" << ret << ")" << std::endl;
    return false;
  }

  return true;
}

int airspy_source_c::work( int noutput_items,
                        gr_vector_const_void_star &input_items,
                        gr_vector_void_star &output_items )
{
  gr_complex *out = (gr_complex *)output_items[0];

  bool running = false;

  if ( _dev )
    running = (airspy_is_streaming( _dev ) == AIRSPY_TRUE);

  if ( ! running )
    return WORK_DONE;

  boost::unique_lock<boost::mutex> lock(_fifo_lock);

  /* Wait until we have the requested number of samples */
  int n_samples_avail = _fifo->size();

  while (n_samples_avail < noutput_items) {
    _samp_avail.wait(lock);
    n_samples_avail = _fifo->size();
  }

  for(int i = 0; i < noutput_items; ++i) {
    out[i] = _fifo->at(0);
    _fifo->pop_front();
  }

  //std::cerr << "-" << std::flush;

  return noutput_items;
}

std::vector<std::string> airspy_source_c::get_devices()
{
  std::vector<std::string> devices;
  std::string label;
#if 0
  for (unsigned int i = 0; i < 1 /* TODO: missing libairspy api */; i++) {
    std::string args = "airspy=" + boost::lexical_cast< std::string >( i );

    label.clear();

    label = "AirSpy"; /* TODO: missing libairspy api */

    boost::algorithm::trim(label);

    args += ",label='" + label + "'";
    devices.push_back( args );
  }
#else

  {
    boost::mutex::scoped_lock lock( _usage_mutex );

    if ( _usage == 0 )
      airspy_init(); /* call only once before the first open */

    _usage++;
  }

  int ret;
  airspy_device *dev = NULL;
  ret = airspy_open(&dev);
  if ( AIRSPY_SUCCESS == ret )
  {
    std::string args = "airspy=0";

    label = "AirSpy";

    uint8_t board_id;
    ret = airspy_board_id_read( dev, &board_id );
    if ( AIRSPY_SUCCESS == ret )
    {
      label += std::string(" ") + airspy_board_id_name(airspy_board_id(board_id));
    }

    args += ",label='" + label + "'";
    devices.push_back( args );

    ret = airspy_close(dev);
  }

  {
    boost::mutex::scoped_lock lock( _usage_mutex );

     _usage--;

    if ( _usage == 0 )
      airspy_exit(); /* call only once after last close */
  }

#endif
  return devices;
}

size_t airspy_source_c::get_num_channels()
{
  return 1;
}

osmosdr::meta_range_t airspy_source_c::get_sample_rates()
{
  osmosdr::meta_range_t range;

  range += osmosdr::range_t( 10e6 );

  return range;
}

double airspy_source_c::set_sample_rate( double rate )
{
  int ret = AIRSPY_SUCCESS;

  if (_dev) {
//    ret = airspy_set_sample_rate( _dev, rate );
    if ( AIRSPY_SUCCESS == ret ) {
      //_sample_rate = rate;
      _sample_rate = get_sample_rates().start();
    } else {
      AIRSPY_THROW_ON_ERROR( ret, AIRSPY_FUNC_STR( "airspy_set_sample_rate", rate ) )
    }
  }

  return get_sample_rate();
}

double airspy_source_c::get_sample_rate()
{
  return _sample_rate;
}

osmosdr::freq_range_t airspy_source_c::get_freq_range( size_t chan )
{
  osmosdr::freq_range_t range;

  range += osmosdr::range_t( 24e6, 1766e6 );

  return range;
}

double airspy_source_c::set_center_freq( double freq, size_t chan )
{
  int ret;

  #define APPLY_PPM_CORR(val, ppm) ((val) * (1.0 + (ppm) * 0.000001))

  if (_dev) {
    double corr_freq = APPLY_PPM_CORR( freq, _freq_corr );
    ret = airspy_set_freq( _dev, uint64_t(corr_freq) );
    if ( AIRSPY_SUCCESS == ret ) {
      _center_freq = freq;
    } else {
      AIRSPY_THROW_ON_ERROR( ret, AIRSPY_FUNC_STR( "airspy_set_freq", corr_freq ) )
    }
  }

  return get_center_freq( chan );
}

double airspy_source_c::get_center_freq( size_t chan )
{
  return _center_freq;
}

double airspy_source_c::set_freq_corr( double ppm, size_t chan )
{
  _freq_corr = ppm;

  set_center_freq( _center_freq );

  return get_freq_corr( chan );
}

double airspy_source_c::get_freq_corr( size_t chan )
{
  return _freq_corr;
}

std::vector<std::string> airspy_source_c::get_gain_names( size_t chan )
{
  std::vector< std::string > names;

  names += "LNA";
  names += "MIX";
  names += "IF";

  return names;
}

osmosdr::gain_range_t airspy_source_c::get_gain_range( size_t chan )
{
  return get_gain_range( "LNA", chan );
}

osmosdr::gain_range_t airspy_source_c::get_gain_range( const std::string & name, size_t chan )
{
  if ( "LNA" == name ) {
    return osmosdr::gain_range_t( 0, 15, 1 );
  }

  if ( "MIX" == name ) {
    return osmosdr::gain_range_t( 0, 15, 1 );
  }

  if ( "IF" == name ) {
    return osmosdr::gain_range_t( 0, 15, 1 );
  }

  return osmosdr::gain_range_t();
}

bool airspy_source_c::set_gain_mode( bool automatic, size_t chan )
{
  _auto_gain = automatic;

  return get_gain_mode(chan);
}

bool airspy_source_c::get_gain_mode( size_t chan )
{
  return _auto_gain;
}

double airspy_source_c::set_gain( double gain, size_t chan )
{
  int ret = AIRSPY_SUCCESS;
  osmosdr::gain_range_t gains = get_gain_range( "LNA", chan );

  if (_dev) {
    double clip_gain = gains.clip( gain, true );
    uint8_t value = clip_gain;

    ret = airspy_set_lna_gain( _dev, value );
    if ( AIRSPY_SUCCESS == ret ) {
      _lna_gain = clip_gain;
    } else {
//      AIRSPY_THROW_ON_ERROR( ret, AIRSPY_FUNC_STR( "airspy_set_lna_gain", value ) )
    }
  }

  return _lna_gain;
}

double airspy_source_c::set_gain( double gain, const std::string & name, size_t chan)
{
  if ( "LNA" == name ) {
    return set_gain( gain, chan );
  }

  if ( "MIX" == name ) {
    return set_mix_gain( gain, chan );
  }

  if ( "IF" == name ) {
    return set_if_gain( gain, chan );
  }

  return set_gain( gain, chan );
}

double airspy_source_c::get_gain( size_t chan )
{
  return _lna_gain;
}

double airspy_source_c::get_gain( const std::string & name, size_t chan )
{
  if ( "LNA" == name ) {
    return get_gain( chan );
  }

  if ( "MIX" == name ) {
    return _mix_gain;
  }

  if ( "IF" == name ) {
    return _vga_gain;
  }

  return get_gain( chan );
}

double airspy_source_c::set_mix_gain(double gain, size_t chan)
{
  int ret;
  osmosdr::gain_range_t gains = get_gain_range( "MIX", chan );

  if (_dev) {
    double clip_gain = gains.clip( gain, true );
    uint8_t value = clip_gain;

    ret = airspy_set_mixer_gain( _dev, value );
    if ( AIRSPY_SUCCESS == ret ) {
      _mix_gain = clip_gain;
    } else {
//      AIRSPY_THROW_ON_ERROR( ret, AIRSPY_FUNC_STR( "airspy_set_mixer_gain", value ) )
    }
  }

  return _mix_gain;
}

double airspy_source_c::set_if_gain(double gain, size_t chan)
{
  int ret;
  osmosdr::gain_range_t gains = get_gain_range( "MIX", chan );

  if (_dev) {
    double clip_gain = gains.clip( gain, true );
    uint8_t value = clip_gain;

    ret = airspy_set_vga_gain( _dev, value );
    if ( AIRSPY_SUCCESS == ret ) {
      _vga_gain = clip_gain;
    } else {
//      AIRSPY_THROW_ON_ERROR( ret, AIRSPY_FUNC_STR( "airspy_set_vga_gain", value ) )
    }
  }

  return _vga_gain;
}

std::vector< std::string > airspy_source_c::get_antennas( size_t chan )
{
  std::vector< std::string > antennas;

  antennas += get_antenna( chan );

  return antennas;
}

std::string airspy_source_c::set_antenna( const std::string & antenna, size_t chan )
{
  return get_antenna( chan );
}

std::string airspy_source_c::get_antenna( size_t chan )
{
  return "RX";
}

double airspy_source_c::set_bandwidth( double bandwidth, size_t chan )
{
  return get_bandwidth( chan );
}

double airspy_source_c::get_bandwidth( size_t chan )
{
  return 10e6;
}

osmosdr::freq_range_t airspy_source_c::get_bandwidth_range( size_t chan )
{
  osmosdr::freq_range_t bandwidths;

  bandwidths += osmosdr::range_t( get_bandwidth( chan ) );

  return bandwidths;
}
