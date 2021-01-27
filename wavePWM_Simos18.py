#!/usr/bin/env python

# wavePWM.py
# 2016-10-06
# Original Script: Public Domain
# Simos18 SBOOT modifications: with credit to @TinyTuning and @bri3d

import pigpio
from time import sleep
class PWM:

   """
   This class may be used to generate PWM on multiple GPIO
   at the same time.

   The following diagram illustrates PWM for one GPIO.

   1      +------------+             +------------+
          |    GPIO    |             |    GPIO    |
          |<--- on --->|             |<--- on --->|
          |    time    |             |    time    |
   0 -----+            +-------------+            +---------
        on^         off^           on^         off^
     +--------------------------+--------------------------+
     ^                          ^                          ^
     |<------ cycle time ------>|<------ cycle time ------>|
   cycle                      cycle                      cycle
   start                      start                      start

   The underlying PWM frequency is the same for all GPIO and
   is the number of cycles per second (known as Hertz).

   The frequency may be specified in Hertz or by specifying
   the cycle time in microseconds (in which case a frequency
   of 1000000 / cycle time is set).

   set_frequency(frequency)
   set_cycle_time(micros)

   The PWM duty cycle (the proportion of on time to cycle time)
   and the start of the on time within each cycle may be set on
   a GPIO by GPIO basis.

   The GPIO PWM duty cycle may be set as a fraction of the
   cycle time (0-1.0) or as the on time in microseconds.

   set_pulse_length_in_micros(gpio, length) 
   set_pulse_length_in_fraction(gpio, length) 

   The GPIO PWM start time within each cycle may be set as
   a fraction of the cycle time from the cycle start (0-1.0)
   or as the number of microseconds from the start of the cycle.

   set_pulse_start_in_micros(gpio, start)
   set_pulse_start_in_fraction(gpio, start)

   There are two convenience functions to set both settings.

   set_pulse_start_and_length_in_micros(gpio, start, length) 
   set_pulse_start_and_length_in_fraction(gpio, start, length) 

   It doesn't matter whether you use the micros or fraction
   functions.  That is a matter of personal choice.

   pigpio waves are used to generate the PWM.  Note that only
   one wave can be transmitted at a time.  So if waves are being
   used to generate PWM they can't also be used at the same time
   for another purpose.

   A wave is generated of length 1000000/frequency microseconds.
   The GPIO are switched on and off within the wave to set the
   duty cycle for each GPIO.    The wave is repeatedly transmitted.

   Waves have a resolution of one microsecond.

   You will only get the requested frequency if it divides
   exactly into 1000000.

   For example, suppose you want a frequency of 7896 cycles per
   second.  The wave length will be 1000000/7896 or 126 microseconds
   (for an actual frequency of 7936.5) and there will be 126 steps
   between off and fully on.
   """

   _MAX_GPIO=32

   def __init__(self, pi, frequency=1000):
      """
      Instantiate with the Pi.

      Optionally the frequency may be specified in Hertz (cycles
      per second).  The frequency defaults to 1000 Hertz.
      """
      self.pi = pi

      self.frequency = frequency
      self.micros = 1000000.0 / frequency

      self.used = [False]*self._MAX_GPIO
      self.pS = [0.0]*self._MAX_GPIO
      self.pL = [0.0]*self._MAX_GPIO
      self.old_wid = None

      self.stop = False

   def set_frequency(self, frequency):
      """
      Sets the PWM frequency in Hertz.

      The change takes affect when the update function is called.
      """
      self.frequency = float(frequency)
      self.micros = 1000000.0 / self.frequency

   def set_cycle_time(self, micros):
      """
      Sets the PWM frequency by specifying the cycle time
      in microseconds.

      The frequency set will be one million divided by micros.

      The change takes affect when the update function is called.
      """
      self.micros = float(micros)
      self.frequency = 1000000.0 / self.micros

   def get_frequency(self):
      """
      Returns the frequency in Hertz.
      """
      return self.frequency

   def get_cycle_length(self):
      """
      Returns the cycle length in microseconds.
      """
      return self.micros

   def set_pulse_length_in_micros(self, gpio, length):
      """
      Sets the GPIO on for length microseconds per cycle.

      The PWM duty cycle for the GPIO will be:

      (length / cycle length in micros) per cycle

      The change takes affect when the update function is called.
      """
      length %= self.micros

      self.pL[gpio] = length / self.micros

      if not self.used[gpio]:
         self.pi.set_mode(gpio, pigpio.OUTPUT)
         pi.set_pull_up_down(gpio, pigpio.PUD_DOWN)
         self.used[gpio] = True

   def set_pulse_length_in_fraction(self, gpio, length):
      """
      Sets the GPIO on for length fraction of each cycle.

      The GPIO will be on for:

      (cycle length in micros * length) microseconds per cycle

      The change takes affect when the update function is called.
      """
      self.set_pulse_length_in_micros(gpio, self.micros * length)

   def set_pulse_start_in_micros(self, gpio, start):
      """
      Sets the GPIO high at start micros into each cycle.

      The change takes affect when the update function is called.
      """

      start %= self.micros

      self.pS[gpio] = start / self.micros

      if not self.used[gpio]:
         self.pi.set_mode(gpio, pigpio.OUTPUT)
         self.used[gpio] = True

   def set_pulse_start_in_fraction(self, gpio, start):
      """
      Sets the GPIO high at start fraction into each cycle.

      The change takes affect when the update function is called.
      """
      self.set_pulse_start_in_micros(gpio, self.micros * start)

   def set_pulse_start_and_length_in_micros(self, gpio, start, length):
      """
      Sets the pulse start and length of each pulse in units
      of microseconds.

      The change takes affect when the update function is called.
      """
      self.set_pulse_start_in_micros(gpio, start)
      self.set_pulse_length_in_micros(gpio, length)

   def set_pulse_start_and_length_in_fraction(self, gpio, start, length):
      """
      Sets the pulse start and length of each pulse as a
      fraction of the cycle length.

      The change takes affect when the update function is called.
      """
      self.set_pulse_start_in_fraction(gpio, start)
      self.set_pulse_length_in_fraction(gpio, length)

   def get_GPIO_settings(self, gpio):
      """
      Returns the pulse start and pulse length as a fraction
      of the cycle time.
      """
      if self.used[gpio]:
         return (True, self.pS[gpio], self.pL[gpio])
      else:
         return (False, 0.0, 0.0)

   def update(self):
      """
      Updates the PWM for each GPIO to reflect the current settings.
      """

      null_wave = True

      for g in range(self._MAX_GPIO):

         if self.used[g]:

            null_wave = False

            on = int(self.pS[g] * self.micros)
            length = int(self.pL[g] * self.micros)

            micros = int(self.micros)

            if length <= 0:
               self.pi.wave_add_generic([pigpio.pulse(0, 1<<g, micros)])
            elif length >= micros:
               self.pi.wave_add_generic([pigpio.pulse(1<<g, 0, micros)])
            else:
               off = (on + length) % micros
               if on < off:
                  self.pi.wave_add_generic([
                     pigpio.pulse(   0, 1<<g,           on),
                     pigpio.pulse(1<<g,    0,     off - on),
                     pigpio.pulse(   0, 1<<g, micros - off),
                     ])
               else:
                  self.pi.wave_add_generic([
                     pigpio.pulse(1<<g,    0,         off),
                     pigpio.pulse(   0, 1<<g,    on - off),
                     pigpio.pulse(1<<g,    0, micros - on),
                     ])

      if not null_wave:

         if not self.stop:

            new_wid = self.pi.wave_create()

            if self.old_wid is not None:

               self.pi.wave_send_using_mode(
                  new_wid, pigpio.WAVE_MODE_REPEAT_SYNC)

               # Spin until the new wave has started.

               while self.pi.wave_tx_at() != new_wid:

                  pass

               # It is then safe to delete the old wave.

               self.pi.wave_delete(self.old_wid)

            else:

               self.pi.wave_send_repeat(new_wid)

            self.old_wid = new_wid

   def cancel(self):
      """
      Cancels PWM on the GPIO.
      """
      self.stop = True

      self.pi.wave_tx_stop()

      if self.old_wid is not None:
         self.pi.wave_delete(self.old_wid)

if __name__ == "__main__":

   import time
   import pigpio
   import wavePWM

   GPIO=[12, 13]

   pi = pigpio.pi()

   if not pi.connected:
      exit(0)

   pwm = wavePWM.PWM(pi)

   pwm.set_frequency(3210) # Using a slightly higher frequency seems to be safer.
   cl = pwm.get_cycle_length()
   pwm.set_pulse_start_in_micros(12, cl/1)
   pwm.set_pulse_length_in_micros(12, cl/2)

   pwm.set_pulse_start_in_micros(13, 3*cl/4)
   pwm.set_pulse_length_in_micros(13, cl/4)
   pwm.update()

   try:
     while True:
       sleep(1)

   except KeyboardInterrupt:
     pass

   print("\ntidying up")

   pwm.cancel()

   pi.stop()

