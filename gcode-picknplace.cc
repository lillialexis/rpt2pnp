/* -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) h.zeller@acm.org. Free Software. GNU Public License v3.0 and above
 */

#include "printer.h"

#include <assert.h>
#include <stdio.h>
#include <math.h>

#include "tape.h"

#include <memory>
#include <map>
#include <iostream>
#include <fstream>
#include <sstream>

// Hovering while transporting a component.
#define Z_HOVERING 10

// Placement needs to be a bit higher.
//#define TAPE_TO_BOARD_DIFFZ 1.6
#define TAPE_TO_BOARD_DIFFZ -2.0

// All templates should be in a separate file somewhere so that we don't
// have to compile.

// Multiplication to get 360 degrees mapped to one turn.
#define ANGLE_FACTOR (50.34965 / 360)

const char *const gcode_preamble = R"(
; Preamble. Fill be whatever is necessary to init.
; Assumes an 'A' axis that rotates the pick'n place nozzle. The values
; 0..360 correspond to absolute degrees.
; (correction: for now, we mess with an E-axis instead of A)
G28 X0 Y0  ; Now home (x/y) - needle over free space
G28 Z0     ; Now it is safe to home z
T1         ; Use E1 extruder
M302
G92 E0

G1 Z35 E0 F2500 ; Move needle out of way
)";

// param: name, x, y, zup, zdown, a, zup
const char *const pick_gcode = R"(
; Pick %s
G1 X%.3f Y%.3f Z%.3f E%.3f ; Move over component to pick.
G1 Z%.3f   ; move down
G4
M42 P6 S255  ; turn on suckage
G1 Z%.3f  ; Move up a bit for traveling
)";

// param: name, x, y, zup, a, zdown, zup
const char *const place_gcode = R"(
; Place %s
G1 X%.3f Y%.3f Z%.3f E%.3f ; Move over component to place.
G1 Z%.3f    ; move down.
G4
M42 P6 S0    ; turn off suckage
G4
M42 P8 S255  ; blow
G4 P100      ; .. for 100ms
M42 P8 S0    ; done.
G1 Z%.3f   ; Move up
)";

// component type -> Tape
struct GCodePickNPlace::Config {
    typedef std::map<std::string, Tape*> PartToTape;

    Position board_origin;  // TODO: potentially rotation...
    PartToTape tape_for_component;
};

GCodePickNPlace::Config *
GCodePickNPlace::ParseConfig(const std::string& filename) {
    std::unique_ptr<Config> result(new Config());

    std::string token;
    float x, y, z;
    Tape* current_tape = NULL;

    std::ifstream in(filename);
    while (result && !in.eof()) {
        token.clear();
        in >> token;

        char buffer[1024];
        in.getline(buffer, sizeof(buffer), '\n');

        if (token.empty() || token[0] == '#')
            continue;

        if (token == "Board:") {
            if (current_tape) current_tape = NULL;
        } else if (token == "Tape:") {
            current_tape = new Tape();
            // This tape is valid for multiple values/footprints possibly.
            // Lets all parse them
            token.clear();
            std::string all_the_names = buffer;
            std::stringstream parts(all_the_names);
            while (!parts.eof()) {
                parts >> token;
                result->tape_for_component[token] = current_tape;
            }
        } else if (token == "origin:") {
            if (current_tape) {
                if (3 != sscanf(buffer, "%f %f %f", &x, &y, &z)) {
                    fprintf(stderr, "Parse problem tape origin: '%s'\n",
                            buffer);
                    result.reset(NULL);
                }
                current_tape->SetFirstComponentPosition(x, y, z);
            } else {
                if (2 != sscanf(buffer, "%f %f",
                                &result->board_origin.x, 
                                &result->board_origin.y)) {
                    fprintf(stderr, "Parse problem board origin: '%s'\n",
                            buffer);
                    result.reset(NULL);
                }
            }
        } else if (token == "spacing:") {
            if (!current_tape) {
                std::cerr << "spacing without tape";
                result.reset(NULL);
                break;
            }
            if (2 != sscanf(buffer, "%f %f", &x, &y)) {
                fprintf(stderr, "Parse problem spacing: '%s'\n", buffer);  // line no ?
                result.reset(NULL);
            }
            if (x == 0 && y == 0) {
                fprintf(stderr, "Spacing: eat least one needs to be set '%s'\n",
                        buffer);  // line no ?
                result.reset(NULL);
            }
            current_tape->SetComponentSpacing(x, y);
        } else if (token == "spacing:") {
            if (!current_tape) {
                std::cerr << "spacing without tape";
                result.reset(NULL);
                break;
            }
            if (1 != sscanf(buffer, "%f", &x)) {
                fprintf(stderr, "Parse problem angle: '%s'\n", buffer);  // line no ?
                result.reset(NULL);
            }
            current_tape->SetAngle(x);
        } else if (token == "count:") {
            if (!current_tape) {
                std::cerr << "Count without tape.";
                result.reset(NULL);
                break;
            }
            int count;
            if (1 != sscanf(buffer, "%d", &count)) {
                fprintf(stderr, "Parse problem count: '%s'.\n", buffer);  // line no ?
                result.reset(NULL);
            }
            current_tape->SetNumberComponents(count);
        }
    }
    return result.release();
}

GCodePickNPlace::GCodePickNPlace(const std::string& filename)
    : config_(ParseConfig(filename)) {
    assert(config_);
#if 0
    fprintf(stderr, "Board-origin: (%.3f, %.3f)\n",
            config_->board_origin.x, config_->board_origin.y);
    for (const auto &t : config_->tape_for_component) {
        fprintf(stderr, "%s\t", t.first.c_str());
        t.second->DebugPrint();
        fprintf(stderr, "\n");
    }
#endif
}

void GCodePickNPlace::Init(const Dimension& dim) {
    printf("%s", gcode_preamble);
}

void GCodePickNPlace::PrintPart(const Part &part) {
    const std::string key = part.footprint + "@" + part.value;
    auto found = config_->tape_for_component.find(key);
    if (found == config_->tape_for_component.end()) {
        fprintf(stderr, "No tape for '%s'\n", key.c_str());
        return;
    }
    Tape *tape = found->second;
    float px, py, pz;
    if (!tape->GetNextPos(&px, &py, &pz)) {
        fprintf(stderr, "We are out of components for '%s'\n", key.c_str());
        return;
    }

    const std::string print_name = part.component_name + " (" + key + ")";
    // param: name, x, y, zdown, a, zup
    printf(pick_gcode,
           print_name.c_str(),
           px, py, pz + Z_HOVERING,                  // component pos.
           ANGLE_FACTOR * fmod(tape->angle(), 360.0),  // pickup angle
           pz,   // down to component
           pz + Z_HOVERING);

    // TODO: right now, we are assuming the z is the same height as
    // param: name, x, y, zup, a, zdown, zup
    printf(place_gcode,
           print_name.c_str(),
           part.pos.x + config_->board_origin.x,
           part.pos.y + config_->board_origin.y, pz + Z_HOVERING,
           ANGLE_FACTOR * fmod(part.angle - tape->angle() + 360, 360.0),
           pz + TAPE_TO_BOARD_DIFFZ,
           pz + Z_HOVERING);
}

void GCodePickNPlace::Finish() {
    printf("\nM84 ; done.\n");
}
