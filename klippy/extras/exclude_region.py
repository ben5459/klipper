# Exclude moves toward and inside set regions
#
# Copyright (C) 2019  Eric Callahan <arksine.code@gmail.com>
#
# This file may be distributed under the terms of the GNU GPLv3 license.

import math
import logging

class ExcludeRegion:
    def __init__(self, config):
        self.printer = config.get_printer()
        self.gcode = self.printer.lookup_object('gcode')
        # Temporary workaround to get skew_correction to register
        # its "klippy:ready" event handler before Exclude Region.  Exclude
        # Region needs to be the highest priority transform, thus it must be
        # the last module that calls set_move_transform()
        if config.has_section('skew_correction'):
            self.printer.try_load_module(config, 'skew_correction')
        # Now ExcludeRegion can register its own event handler
        self.printer.register_event_handler("klippy:ready",
                                            self._handle_ready)
        self.printer.register_event_handler("sdcard:reset_file",
                                            self._handle_reset_file)
        self.objects = []
        self.current_object = ""
        self.in_excluded_region = False
        self.last_position = [0., 0., 0., 0.]
        self.last_position_extruded = [0., 0., 0., 0.]
        self.last_position_excluded = [0., 0., 0., 0.]
        self.gcode.register_command(
            'START_CURRENT_OBJECT', self.cmd_START_CURRENT_OBJECT,
            desc=self.cmd_START_CURRENT_OBJECT_help)
        self.gcode.register_command(
            'END_CURRENT_OBJECT', self.cmd_END_CURRENT_OBJECT,
            desc=self.cmd_END_CURRENT_OBJECT_help)
        self.gcode.register_command(
            'EXCLUDE_OBJECT', self.cmd_EXCLUDE_OBJECT,
            desc=self.cmd_EXCLUDE_OBJECT_help)
        self.gcode.register_command(
            'REMOVE_ALL_EXCLUDED', self.cmd_REMOVE_ALL_EXCLUDED,
            desc=self.cmd_REMOVE_ALL_EXCLUDED_help)
        # debugging
        self.current_region = None
    def _handle_ready(self):
        gcode_move = self.printer.lookup_object('gcode_move')
        self.next_transform = gcode_move.set_move_transform(self, force=True)
    def get_position(self):
        self.last_position[:] = self.next_transform.get_position()
        self.last_delta = [0., 0., 0., 0.]
        return list(self.last_position)

    def _normal_move(self, newpos, speed):
        self.last_position_extruded[:] = newpos
        self.last_position[:] = newpos
        self.next_transform.move(newpos, speed)

    def _ignore_move(self, newpos, speed):
        self.last_position_excluded[:] = newpos
        self.last_position[:] = newpos
        return

    def _move_into_excluded_region(self, newpos, speed):
        logging.info("Moving to excluded object: " + self.current_object)
        self.in_excluded_region = True
        self.last_position_excluded[:] = newpos
        self.last_position[:] = newpos

    def _move_from_excluded_region(self, newpos, speed):
        logging.info("Moving to included object: " + self.current_object)
        logging.info("last position: " + " ".join(str(x) for x in self.last_position))
        logging.info("last extruded position: " + " ".join(str(x) for x in self.last_position_extruded))
        logging.info("last excluded position: " + " ".join(str(x) for x in self.last_position_excluded))
        logging.info("New position: " + " ".join(str(x) for x in newpos))
        newpos[0] = newpos[0] - self.last_position_excluded[0] + self.last_position_extruded[0]
        newpos[1] = newpos[1] - self.last_position_excluded[1] + self.last_position_extruded[1]
        newpos[3] = newpos[3] - self.last_position_excluded[3] + self.last_position_extruded[3]
        logging.info("Modified position: " + " ".join(str(x) for x in newpos))
        self.last_position[:] = newpos
        self.last_position_extruded[:] = newpos
        self.next_transform.move(newpos, speed)
        self.in_excluded_region = False

    def _test_in_excluded_region(self):
        # Inside cancelled object
        if self.current_object in self.objects:
            return True

    def move(self, newpos, speed):
        move_in_excluded_region = self._test_in_excluded_region()

        if move_in_excluded_region:
            if self.in_excluded_region:
                self._ignore_move(newpos, speed)
            else:
                self._move_into_excluded_region(newpos, speed)
        else:
            if self.in_excluded_region:
                self._move_from_excluded_region(newpos, speed)
            else:
                self._normal_move(newpos, speed)

    cmd_START_CURRENT_OBJECT_help = "Marks the beginning the current object as labeled"
    def cmd_START_CURRENT_OBJECT(self, params):
        self.current_object = params.get_command_parameters()['NAME'].upper()
    cmd_END_CURRENT_OBJECT_help = "Markes the end the current object"
    def cmd_END_CURRENT_OBJECT(self, params):
        self.current_object = ""
    cmd_EXCLUDE_OBJECT_help = "Cancel moves inside a specified objects"
    def cmd_EXCLUDE_OBJECT(self, params):
        name = params.get_command_parameters()['NAME'].upper()
        if name not in self.objects:
            self.objects.append(name)
    cmd_REMOVE_ALL_EXCLUDED_help = "Removes all excluded objects and regions"
    def cmd_REMOVE_ALL_EXCLUDED(self, params):
        self._handle_reset_file
    def _handle_reset_file(self):
        self.objects = []

def load_config(config):
    return ExcludeRegion(config)
