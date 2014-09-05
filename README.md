# Introduction

This is a fork of gr-osmosdr (git://git.osmocom.org/gr-osmosdr) which adds support for hackrf_tcp as source.

![gqrx using hackrf_source](http://nixgeneration.com/~jaime/misc/hackrf_tcp-source.png)


hackrf_tcp source was coded in a rush just as a PoC and uploaded to github for the record. It will be abandoned in favour of osmorsdr_tcp which will work with any SDR supported by gr-osmosdr.

Also note that this has only been tested to be working with Linux.

# Known Issues

  * Tested only with Linux
  * It does not support hostnames at the "hackrf_tcp" parameter. Only IP addresses for now


# Dependencies

  * libhackrf https://github.com/mossmann/hackrf

# Build

    $ mkdir build
    $ cd build/
    $ cmake -DCMAKE_INSTALL_PREFIX=/opt/gnuradio/ ../
    $ make
    # make install
    
