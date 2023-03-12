.. _openAMP_rsc_table_sample:

OpenAMP Sample Application using resource table
###############################################

Overview
********

This application demonstrates how to use OpenAMP with Zephyr based on a resource
table. It is designed to respond to the `Linux rpmsg tty driver <https://elixir.bootlin.com/linux/latest/source/drivers/tty/rpmsg_tty.c>`_.
This sample implementation is compatible with platforms that embed
a Linux kernel OS on the main processor and a Zephyr application on
the co-processor.

Building the application
*************************

Zephyr
-------

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/ipc/openamp_rsc_table
   :goals: test

Linux
------

Enable RPMSG_TTY configuration to build and install
the rpmsg_tty.ko module on the target.

Running the sample
*******************
Linux console
---------------

Open a Linux shell (minicom, ssh, etc.) and insert a module into the Linux Kernel

.. code-block:: console

   root@linuxshell: modprobe rpmsg_tty
   root@linuxshell: echo -n zephyr_openamp_rsc_table.elf /sys/class/remoteproc/remoteproc0/firmware
   root@linuxshell: echo start > /sys/class/remoteproc/remoteproc0/state # Start co-processor

Result on Zephyr console
-------------------------

The following message will appear on the corresponding Zephyr console:

.. code-block:: console

   root@linuxshell: tail -f /sys/kernel/debug/remoteproc/remoteproc0/trace0 # For zehpyr console

.. code-block:: console

   ***** Booting Zephyr OS v#.##.#-####-g########## *****

   OpenAMP[remote] Linux responder demo started
   OpenAMP[remote] Linux tty responder started

rpmsg TTY demo on Linux console
-------------------------------

On the Linux console send a message to Zephyr which answers with the "TTY <add>" prefix.
<addr> corresponds to the Zephyr rpmsg-tty endpoint address:

Led Management :
Send json for enable/disable led

.. code-block:: console

   $> cat /dev/ttyRPMSG0 &
   $> echo "{\"id\":\"led\",\"data\":0}" > /dev/ttyRPMSG0 # To disable LED
   $> echo "{\"id\":\"led\",\"data\":1}" > /dev/ttyRPMSG0 # To enable LED
   
   tty: { "status": true}  # co-processor response when led gpio is properly use
   tty: { "status": false} # co-processor response when led gpio isn't properly use or wrong json send

Button Management :
Press button :

.. code-block:: console

   $> cat /dev/ttyRPMSG0 &
   tty: { "button": true}
