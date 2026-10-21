# The game data build: out/<T>/ (BIGLUMP.BIN, the generated headers, the CD
# files the PC exe reads), on CMake + Ninja (issue #95).
#
# This replaces the 1999 makefile.gfx route (MSYS2 make + Perl, through
# port/build-data.sh).  The converters and their arguments are the ones that
# makefile ran (lists.cmake), so the output is the same, byte for byte.  What
# changes is around them:
# - every step declares what it reads and writes, its converter included,
#   so a rebuild redoes exactly what changed;
# - steps run in parallel: no temp file is shared (each screen's is its
#   own), and MkActor - whose Actor.Tmp/Actor.Pak live in its working
#   directory - runs one at a time;
# - the string order is the English file's (port/tools/data/lang.py), not
#   Perl's hash order;
# - there are no generated makefiles to rebuild every other run.
#
# No compiler: the converters are the committed port/tools/x64 exes (or,
# with SBSP_DATA_TOOLS=vintage, the 1999 ones), built by port/CMakeLists.txt
# ahead of time.  Each one runs from the repo root, as under make: scripts
# #include their defs by repo-relative path, MkLevel.ini's BackGfxDir is
# relative, and parkgrab sorts by the paths it is given.
#
# port/build-data.cmd / .sh drive it (port/tools/data/build_data.py):
#   cmake --preset data-usa && cmake --build --preset data-usa
cmake_minimum_required(VERSION 3.25)
project(sbsp_data NONE)

get_filename_component(SBSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(SBSP_TERRITORY USA CACHE STRING "Territory: USA or EUR")
set_property(CACHE SBSP_TERRITORY PROPERTY STRINGS USA EUR)
set(SBSP_DATA_TOOLS x64 CACHE STRING "Converters: x64 (port/tools/x64) or vintage (the 1999 exes)")
set_property(CACHE SBSP_DATA_TOOLS PROPERTY STRINGS x64 vintage)
set(SBSP_DATA_FINAL_DAT "" CACHE FILEPATH
    "Testing only: transtext reads this final.dat instead of lang.py's (an A/B against the make build's)")
if(NOT SBSP_TERRITORY MATCHES "^(USA|EUR)$")
    message(FATAL_ERROR "SBSP_TERRITORY is '${SBSP_TERRITORY}': USA or EUR")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(PY ${Python3_EXECUTABLE} -I)
set(DATA_PY ${SBSP_ROOT}/port/tools/data)

# --- Converters --------------------------------------------------------------
# Paths from the repo root.  The x64 MkLevel reads MkLevel.ini from -c:; the
# 1999 one from beside its exe.  MkActor packs frames through
# system("lznp.exe ..."), found on the PATH (datastep.py run).
set(_tools MkData Tga2Gfx scripter lznp MkActor MkLevel parkgrab transtext)
if(SBSP_DATA_TOOLS STREQUAL "x64")
    foreach(_t IN LISTS _tools)
        set(TOOL_${_t} port/tools/x64/${_t}.exe)
    endforeach()
    set(MKLEVEL_CFG -c:tools/data/bin)
elseif(SBSP_DATA_TOOLS STREQUAL "vintage")
    foreach(_t MkData Tga2Gfx scripter parkgrab transtext)
        set(TOOL_${_t} tools/${_t}.exe)
    endforeach()
    set(TOOL_MkLevel tools/data/bin/MkLevel.exe)
    set(TOOL_MkActor tools/data/bin/MkActor.exe)
    set(TOOL_lznp port/tools/lznp.exe)	# the 16-bit original cannot run
    set(MKLEVEL_CFG)
else()
    message(FATAL_ERROR "SBSP_DATA_TOOLS is '${SBSP_DATA_TOOLS}': x64 or vintage")
endif()
foreach(_t IN LISTS _tools)
    set(_exe ${SBSP_ROOT}/${TOOL_${_t}})
    if(NOT EXISTS ${_exe})
        message(FATAL_ERROR "${TOOL_${_t}} is missing")
    endif()
    file(SIZE ${_exe} _size)
    if(_size LESS 4096)
        message(FATAL_ERROR "${TOOL_${_t}} is ${_size} bytes, an unmaterialised Git LFS pointer: "
                            "git lfs pull --include \"port/tools/x64/*.exe,port/tools/lznp.exe\"")
    endif()
    set(EXE_${_t} ${_exe})
endforeach()
get_filename_component(LZNP_DIR ${EXE_lznp} DIRECTORY)

include(lists.cmake)

set(OUT_REL out/${SBSP_TERRITORY})
set(OUT ${SBSP_ROOT}/${OUT_REL})
set(MKDIR ${CMAKE_COMMAND} -E make_directory)
set_property(GLOBAL PROPERTY JOB_POOLS mkactor=1)
set(ALL_OUTPUTS)		# what `data` builds
set(PACKED)				# what BIGLUMP.BIN is built after (data/DataCache.scr lists it)

# --- Text: lang.py -> transtext -> trans_scr.py ---------------------------------
# One step: transtext writes every language's .dat, id.dat and trans.h from
# one final.dat, and trans.scr comes from that trans.h.  trans.scr stays where
# every script #includes it, shared by the territories; trans_scr.py writes
# it only when its content changes.
set(TR ${OUT}/data/translations)
set(TRANS_H ${OUT}/include/trans.h)
set(TRANS_SCR ${SBSP_ROOT}/data/Scripts/defs/trans.scr)
set(_dats)
foreach(_l eng fre ger ita spa dut swe jap id)
    list(APPEND _dats ${TR}/${_l}.dat)
endforeach()
set(_langIn)
foreach(_l IN LISTS LANGUAGES)
    list(APPEND _langIn data/translations/${_l}.dat)
endforeach()
list(TRANSFORM _langIn PREPEND ${SBSP_ROOT}/ OUTPUT_VARIABLE _langDeps)
if(SBSP_DATA_FINAL_DAT)
    set(_lang COMMAND ${CMAKE_COMMAND} -E copy ${SBSP_DATA_FINAL_DAT} ${TR}/final.dat)
    set(_langDeps ${SBSP_DATA_FINAL_DAT})
else()
    set(_lang COMMAND ${PY} ${DATA_PY}/lang.py data/translations/text.dat
                      ${OUT_REL}/data/translations/final.dat ${_langIn})
    list(APPEND _langDeps ${SBSP_ROOT}/data/translations/text.dat ${DATA_PY}/lang.py)
endif()
add_custom_command(OUTPUT ${TR}/final.dat ${_dats} ${TRANS_H} ${TRANS_SCR}
    COMMAND ${MKDIR} ${TR} ${OUT}/include
    ${_lang}
    COMMAND ${EXE_transtext} -h:${OUT_REL}/include/trans.h -o:${OUT_REL}/data/translations
            ${OUT_REL}/data/translations/final.dat
    COMMAND ${PY} ${DATA_PY}/trans_scr.py ${OUT_REL}/include/trans.h data/scripts/defs/trans.scr STR__INGAME__
    DEPENDS ${_langDeps} ${DATA_PY}/trans_scr.py ${EXE_transtext}
    WORKING_DIRECTORY ${SBSP_ROOT} VERBATIM
    COMMENT "Translations: final.dat, the language .dat files, trans.h, trans.scr")
list(APPEND ALL_OUTPUTS ${TR}/final.dat ${_dats} ${TRANS_H} ${TRANS_SCR})
list(APPEND PACKED ${_dats})

# --- Scripts (scripter) ------------------------------------------------------
file(GLOB _defs CONFIGURE_DEPENDS ${SBSP_ROOT}/data/Scripts/defs/*.scr)
list(FILTER _defs EXCLUDE REGEX "/[Tt][Rr][Aa][Nn][Ss]\\.[Ss][Cc][Rr]$")
foreach(_s IN LISTS SCRIPTS_LIST)
    set(_o ${OUT}/data/scripts/${_s}.dat)
    add_custom_command(OUTPUT ${_o}
        COMMAND ${MKDIR} ${OUT}/data/scripts
        COMMAND ${EXE_scripter} data/scripts/${_s}.scr ${OUT_REL}/data/scripts/${_s}.dat
        DEPENDS ${SBSP_ROOT}/data/scripts/${_s}.scr ${_defs} ${TRANS_SCR} ${EXE_scripter}
        WORKING_DIRECTORY ${SBSP_ROOT} VERBATIM
        COMMENT "scripter ${_s}")
    list(APPEND ALL_OUTPUTS ${_o})
    list(APPEND PACKED ${_o})
endforeach()

# --- The sprite bank, shop and party (parkgrab) ------------------------------------
# sprites.rsp is the file list makefile.gfx wrote with $(file): parkgrab reads
# @file as the words of one line, switches included, and expands the
# sprite actors' globs itself.
string(JOIN " " _rsp ${SPRITES_WORDS})
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/sprites.rsp CONTENT "${_rsp}\n" NEWLINE_STYLE UNIX)
set(SPRITES_H ${OUT}/include/Sprites.h)
set(_spr ${OUT_REL}/data/sprites)
list(TRANSFORM SPRITES_FILES PREPEND ${SBSP_ROOT}/ OUTPUT_VARIABLE _sprDeps)
add_custom_command(OUTPUT ${OUT}/data/sprites/sprites.spr ${SPRITES_H}
                          ${OUT}/data/sprites/sprites.rsp ${OUT}/data/sprites/sprites.lbm
    COMMAND ${MKDIR} ${OUT}/data/sprites ${OUT}/include
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/sprites.rsp ${OUT}/data/sprites/sprites.rsp
    COMMAND ${PY} ${DATA_PY}/datastep.py parkgrab ${_spr}/sprites.h ${OUT_REL}/include/Sprites.h --
            ${EXE_parkgrab} -b+ @${_spr}/sprites.rsp -t:12,4,1 -l:${_spr}/sprites.lbm -o:${_spr}/Sprites.Spr
    DEPENDS ${CMAKE_BINARY_DIR}/sprites.rsp ${_sprDeps} ${SPRITES_ACTOR_DEPS} ${EXE_parkgrab} ${DATA_PY}/datastep.py
    WORKING_DIRECTORY ${SBSP_ROOT} VERBATIM
    COMMENT "parkgrab: the sprite bank and Sprites.h")
list(APPEND ALL_OUTPUTS ${OUT}/data/sprites/sprites.spr ${SPRITES_H})
list(APPEND PACKED ${OUT}/data/sprites/sprites.spr)

list(TRANSFORM SHOP_GFX_TRANS PREPEND graphics/shop/ OUTPUT_VARIABLE _shop)
list(TRANSFORM _shop APPEND .bmp)
list(TRANSFORM _shop PREPEND ${SBSP_ROOT}/ OUTPUT_VARIABLE _shopDeps)
add_custom_command(OUTPUT ${OUT}/data/shop/shop.spr ${OUT}/include/shop.h ${OUT}/report/shop.lbm
    COMMAND ${MKDIR} ${OUT}/data/shop ${OUT}/include ${OUT}/report
    COMMAND ${PY} ${DATA_PY}/datastep.py parkgrab ${OUT_REL}/data/shop/shop.h ${OUT_REL}/include/shop.h --
            ${EXE_parkgrab} -z+ ${_shop} -b+ -t:8,1,1 -l:${OUT_REL}/report/shop.lbm -o:${OUT_REL}/data/shop/shop.spr
    DEPENDS ${_shopDeps} ${EXE_parkgrab} ${DATA_PY}/datastep.py
    WORKING_DIRECTORY ${SBSP_ROOT} VERBATIM
    COMMENT "parkgrab: shop")
list(TRANSFORM PARTY_GFX_TRANS PREPEND graphics/party/ OUTPUT_VARIABLE _party)
list(TRANSFORM _party APPEND .bmp)
list(TRANSFORM _party PREPEND ${SBSP_ROOT}/ OUTPUT_VARIABLE _partyDeps)
add_custom_command(OUTPUT ${OUT}/data/party/party.spr ${OUT}/include/party.h ${OUT}/report/party.lbm
                          ${OUT}/data/party/rep.txt
    COMMAND ${MKDIR} ${OUT}/data/party ${OUT}/include ${OUT}/report
    COMMAND ${PY} ${DATA_PY}/datastep.py parkgrab ${OUT_REL}/data/party/party.h ${OUT_REL}/include/party.h --
            ${EXE_parkgrab} -c- -z+ -r+ -q+ ${_party} -b+ -t:8,4,1 -l:${OUT_REL}/report/party.lbm
            -o:${OUT_REL}/data/party/party.spr -k:${OUT_REL}/data/party/rep.txt
    DEPENDS ${_partyDeps} ${EXE_parkgrab} ${DATA_PY}/datastep.py
    WORKING_DIRECTORY ${SBSP_ROOT} VERBATIM
    COMMENT "parkgrab: party")
list(APPEND ALL_OUTPUTS ${OUT}/data/shop/shop.spr ${OUT}/include/shop.h
                        ${OUT}/data/party/party.spr ${OUT}/include/party.h)
list(APPEND PACKED ${OUT}/data/shop/shop.spr ${OUT}/data/party/party.spr)

# --- Levels (MkLevel) --------------------------------------------------------
# A level reads its .mex, the tiles and textures in its folder, the hazards
# and platforms it places (graphics/hazards, graphics/Platforms), the
# backgrounds MkLevel.ini names (graphics/levels/BackGfx) and the ini.  A
# shared texture's change rebuilds all 27, which is cheap.  Sprites.h is the
# #43 order: a level comes after the sprite bank it draws from.
file(GLOB_RECURSE _levelShared CONFIGURE_DEPENDS
     ${SBSP_ROOT}/graphics/hazards/* ${SBSP_ROOT}/graphics/Platforms/* ${SBSP_ROOT}/graphics/levels/BackGfx/*)
foreach(_ch IN LISTS LEVELS_CHAPTERS)
    foreach(_lv IN LISTS LEVELS_${_ch})
        set(_n ${_ch}_${_lv})
        file(GLOB_RECURSE _levelDeps CONFIGURE_DEPENDS ${SBSP_ROOT}/graphics/levels/${_ch}/${_lv}/*)
        set(_o ${OUT}/data/levels/${_n}.Lvl)
        add_custom_command(OUTPUT ${_o}
            BYPRODUCTS ${OUT}/data/levels/${_n}.Lbm ${OUT}/data/levels/${_n}.tex ${OUT}/data/levels/${_n}.h
                       ${OUT}/include/${_n}_INF.h
            COMMAND ${MKDIR} ${OUT}/data/levels ${OUT}/include
            COMMAND ${EXE_MkLevel} ${MKLEVEL_CFG} graphics/levels/${_ch}/${_lv}/${_lv}.mex
                    -o:${OUT_REL}/data/levels/${_n}.lvl -i:${OUT_REL}/include ${LEVELS_OPTS} ${${_n}_OPTS}
            DEPENDS ${_levelDeps} ${_levelShared} ${SBSP_ROOT}/tools/data/bin/MkLevel.ini ${SPRITES_H} ${EXE_MkLevel}
            WORKING_DIRECTORY ${SBSP_ROOT} VERBATIM
            COMMENT "MkLevel ${_n}")
        list(APPEND ALL_OUTPUTS ${_o})
        list(APPEND PACKED ${_o})
    endforeach()
endforeach()

# --- Actors (MkActor) --------------------------------------------------------
foreach(_a IN LISTS ACTOR_LIST)
    string(TOUPPER ${_a} _A)
    file(GLOB_RECURSE _actorDeps CONFIGURE_DEPENDS ${SBSP_ROOT}/graphics/Characters/${_a}/*)
    set(_o ${OUT}/data/Actors/${_A}.SBK)
    add_custom_command(OUTPUT ${_o}
        BYPRODUCTS ${OUT}/include/ACTOR_${_A}_Anim.h
        COMMAND ${MKDIR} ${OUT}/data/Actors ${OUT}/include
        COMMAND ${PY} ${DATA_PY}/datastep.py run ${LZNP_DIR} --
                ${EXE_MkActor} -r:graphics/Characters -s:Render/Psx -o:${OUT_REL}/data/Actors
                -i:${OUT_REL}/include ${_a} -c:16
        DEPENDS ${_actorDeps} ${EXE_MkActor} ${EXE_lznp} ${DATA_PY}/datastep.py
        WORKING_DIRECTORY ${SBSP_ROOT} VERBATIM
        JOB_POOL mkactor
        COMMENT "MkActor ${_a}")
    list(APPEND ALL_OUTPUTS ${_o})
    list(APPEND PACKED ${_o})
endforeach()

# --- Full-screen images (Tga2Gfx + lznp) -------------------------------------
function(sbsp_screens in_dir out_dir)
    foreach(_s IN LISTS ARGN)
        set(_o ${OUT}/data/${out_dir}/${_s}.gfx)
        add_custom_command(OUTPUT ${_o}
            COMMAND ${MKDIR} ${OUT}/data/${out_dir}
            COMMAND ${PY} ${DATA_PY}/datastep.py gfx ${EXE_Tga2Gfx} ${EXE_lznp}
                    graphics/${in_dir}/${_s}.tga ${OUT_REL}/data/${out_dir}/${_s}.gfx
            DEPENDS ${SBSP_ROOT}/graphics/${in_dir}/${_s}.tga ${EXE_Tga2Gfx} ${EXE_lznp} ${DATA_PY}/datastep.py
            WORKING_DIRECTORY ${SBSP_ROOT} VERBATIM
            COMMENT "Tga2Gfx ${out_dir}/${_s}")
        list(APPEND ALL_OUTPUTS ${_o})
        list(APPEND PACKED ${_o})
    endforeach()
    set(ALL_OUTPUTS ${ALL_OUTPUTS} PARENT_SCOPE)
    set(PACKED ${PACKED} PARENT_SCOPE)
endfunction()
sbsp_screens(backdrop backdrop ${BACKDROPS})
sbsp_screens(ui/map ui/map ${MAPSCREENS})
sbsp_screens(loadingscreens loadingscreens ${LOADINGSCREENS})

# --- Copies: SFX and music banks, the memory card header, the demo -----------
function(sbsp_copy src dst)
    add_custom_command(OUTPUT ${OUT}/data/${dst}
        COMMAND ${CMAKE_COMMAND} -E copy ${SBSP_ROOT}/${src} ${OUT}/data/${dst}
        DEPENDS ${SBSP_ROOT}/${src} VERBATIM
        COMMENT "copy ${dst}")
    list(APPEND ALL_OUTPUTS ${OUT}/data/${dst})
    list(APPEND PACKED ${OUT}/data/${dst})
    set(ALL_OUTPUTS ${ALL_OUTPUTS} PARENT_SCOPE)
    set(PACKED ${PACKED} PARENT_SCOPE)
endfunction()
foreach(_m IN LISTS SFX_LIST)
    foreach(_e PXM VH VB)
        sbsp_copy(data/SFX/${_m}/${_m}.${_e} SFX/${_m}.${_e})
    endforeach()
endforeach()
foreach(_m IN LISTS MUSIC_LIST)
    foreach(_e PXM VH VB)
        sbsp_copy(data/music/${_m}/${_m}.${_e} music/${_m}.${_e})
    endforeach()
endforeach()
sbsp_copy(data/memcard/memhead.bin memcard/memhead.bin)
foreach(_d IN LISTS DEMO_LIST)
    sbsp_copy(data/demo/${_d}.dmo demo/${_d}.dmo)
endforeach()

# --- BIGLUMP.BIN + BigLump.h (MkData), and the CD files the PC exe reads -----------
# data/DataCache.scr lists what goes in, in order.  The lump goes to
# out/<T>/cd, where the PC exe reads it, and to both PS1 trees'
# version/CD, where makefile.gaz's cddata looks for it (the data doesn't
# depend on DEBUG/FINAL).
set(LUMP ${OUT}/cd/BIGLUMP.BIN)
add_custom_command(OUTPUT ${LUMP} ${OUT}/include/BigLump.h
                          ${OUT}/DEBUG/version/CD/BIGLUMP.BIN ${OUT}/FINAL/version/CD/BIGLUMP.BIN
    COMMAND ${MKDIR} ${OUT}/cd ${OUT}/include ${OUT}/DEBUG/version/CD ${OUT}/FINAL/version/CD
    COMMAND ${EXE_MkData} -b:${OUT_REL}/Data/ -s:data/DataCache.Scr -o:${OUT_REL}/cd/BigLump.Bin
            -i:${OUT_REL}/include/BigLump.h
    COMMAND ${CMAKE_COMMAND} -E copy ${LUMP} ${OUT}/DEBUG/version/CD/BIGLUMP.BIN
    COMMAND ${CMAKE_COMMAND} -E copy ${LUMP} ${OUT}/FINAL/version/CD/BIGLUMP.BIN
    DEPENDS ${SBSP_ROOT}/data/DataCache.scr ${PACKED} ${EXE_MkData}
    WORKING_DIRECTORY ${SBSP_ROOT} VERBATIM
    COMMENT "MkData: BIGLUMP.BIN and BigLump.h")
list(APPEND ALL_OUTPUTS ${LUMP} ${OUT}/include/BigLump.h
                        ${OUT}/DEBUG/version/CD/BIGLUMP.BIN ${OUT}/FINAL/version/CD/BIGLUMP.BIN)

# the streamed audio and the movies, from data/CDData (Git LFS)
foreach(_f Track1.Ixa:TRACK1.IXA thq.str:THQ.STR climax.str:CLIMAX.STR intro.str:INTRO.STR demo.str:DEMO.STR)
    string(REPLACE ":" ";" _f ${_f})
    list(GET _f 0 _src)
    list(GET _f 1 _dst)
    add_custom_command(OUTPUT ${OUT}/cd/${_dst}
        COMMAND ${MKDIR} ${OUT}/cd
        COMMAND ${PY} ${DATA_PY}/datastep.py stage data/CDData/${_src} ${OUT_REL}/cd/${_dst}
        DEPENDS ${SBSP_ROOT}/data/CDData/${_src} ${DATA_PY}/datastep.py
        WORKING_DIRECTORY ${SBSP_ROOT} VERBATIM
        COMMENT "stage cd/${_dst}")
    list(APPEND ALL_OUTPUTS ${OUT}/cd/${_dst})
endforeach()

add_custom_target(data ALL DEPENDS ${ALL_OUTPUTS})
