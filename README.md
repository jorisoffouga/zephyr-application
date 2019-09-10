# zephyr-application

I. Download Zephyr-Application
============

It's recommend to create your own python virtualenv
```
$: mkdir zephyryproject
$: cd zephyryproject
$: python3 -m venv zephyr-venv
$: source zephyr-venv/bin/activate
```
To get the module you need to have `west` installed and use it as:

Install the `west` utility:

```
$: pip install west
```
Download the Zephyr-Application sources (Including Zephyr master revision):

```
$: west init -m https://github.com/jorisoffouga/zephyr-application.git
$: west update
```
At the end of the commands you have every metadata you need to start work with.

II. Install dependancies
=======

See [Zephyr Documentation](https://docs.zephyrproject.org/latest/getting_started/index.html)
