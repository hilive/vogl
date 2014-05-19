/**************************************************************************
 *
 * Copyright 2013-2014 RAD Game Tools and Valve Software
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 **************************************************************************/

// File: vogl_tool_replay.cpp
#include "vogl_common.h"
#include "vogl_gl_replayer.h"
#include "vogl_file_utils.h"
#include "vogl_find_files.h"

#include "libtelemetry.h"

#if (!VOGL_PLATFORM_HAS_X11)

    bool tool_replay_mode()
    {
        VOGL_VERIFY(!"impl tool_replay_mode");
        return false;
    }

#else

struct replay_data_t
{
    replay_data_t() :
        trim_call_index(-1),
        wmDeleteMessage(None),
        win_mapped(false),
        paused_mode_frame_index(-1),
        take_snapshot_at_frame_index(-1),
        slow_mode(false),
        paused_mode(false),
        pSnapshot(NULL),
        snapshot_loop_start_frame(-1),
        snapshot_loop_end_frame(-1),
        write_snapshot_index(-1),
        multitrim_mode(false),
        multitrim_interval(0),
        multitrim_frames_remaining(0),
        num_trim_files_written(0),
        highest_frame_to_trim(0),
        loop_frame(0),
        loop_len(0),
        loop_count(0),
        draw_kill_max_thresh(0),
        endless_mode(false)
    {}

    timer tm;

    vogl_unique_ptr<vogl_trace_file_reader> pTrace_reader;

    vogl_loose_file_blob_manager trim_file_blob_manager;

    vogl::vector<dynamic_string> trim_filenames;
    vogl::vector<uint32_t> trim_frames;
    vogl::vector<uint32_t> trim_lens;
    int64_t trim_call_index;

    dynamic_string trace_filename;
    dynamic_string keyframe_base_filename;
    vogl::vector<uint64_t> keyframes;

    vogl_gl_replayer replayer;
    vogl_replay_window window;
    Atom wmDeleteMessage;

    Bool win_mapped;
    vogl::hash_map<uint64_t> keys_down;
    vogl::hash_map<uint64_t> keys_pressed;

    int64_t paused_mode_frame_index;
    int64_t take_snapshot_at_frame_index;
    bool slow_mode;
    bool paused_mode;

    dynamic_string write_snapshot_filename;
    vogl_gl_state_snapshot *pSnapshot;
    int64_t snapshot_loop_start_frame;
    int64_t snapshot_loop_end_frame;
    int64_t write_snapshot_index;

    bool multitrim_mode;
    int multitrim_interval;
    int multitrim_frames_remaining;
    uint32_t num_trim_files_written;
    uint32_t highest_frame_to_trim;

    int loop_frame;
    int loop_len;
    int loop_count;
    int draw_kill_max_thresh;
    bool endless_mode;
    bool benchmark_mode;
    bool benchmark_mode_allow_state_teardown;
};

//----------------------------------------------------------------------------------------------------------------------
// X11_Pending - from SDL
//----------------------------------------------------------------------------------------------------------------------
static int X11_Pending(Display *display)
{
    VOGL_FUNC_TRACER

    /* Flush the display connection and look to see if events are queued */
    XFlush(display);
    if (XEventsQueued(display, QueuedAlready))
        return 1;

    /* More drastic measures are required -- see if X is ready to talk */
    {
        static struct timeval zero_time; /* static == 0 */
        int x11_fd;
        fd_set fdset;

        x11_fd = ConnectionNumber(display);
        FD_ZERO(&fdset);
        FD_SET(x11_fd, &fdset);
        if (select(x11_fd + 1, &fdset, NULL, NULL, &zero_time) == 1)
        {
            return (XPending(display));
        }
    }

    /* Oh well, nothing is ready .. */
    return 0;
}

//----------------------------------------------------------------------------------------------------------------------
// read_state_snapshot_from_trace
//----------------------------------------------------------------------------------------------------------------------
static vogl_gl_state_snapshot *read_state_snapshot_from_trace(dynamic_string filename)
{
    VOGL_FUNC_TRACER

    timed_scope ts(VOGL_FUNCTION_INFO_CSTR);

    vogl_gl_state_snapshot *pSnapshot = NULL;

    vogl_loose_file_blob_manager file_blob_manager;
    dynamic_string keyframe_trace_path(file_utils::get_pathname(filename.get_ptr()));
    file_blob_manager.init(cBMFReadable, keyframe_trace_path.get_ptr());

    dynamic_string actual_keyframe_filename;
    vogl_unique_ptr<vogl_trace_file_reader> pTrace_reader(vogl_open_trace_file(filename, actual_keyframe_filename, NULL));
    if (!pTrace_reader.get())
    {
        vogl_error_printf("%s: Failed reading keyframe file %s!\n", VOGL_FUNCTION_INFO_CSTR, filename.get_ptr());
        return NULL;
    }

    bool found_snapshot = false;
    vogl_ctypes trace_gl_ctypes(pTrace_reader->get_sof_packet().m_pointer_sizes);
    vogl_trace_packet keyframe_trace_packet(&trace_gl_ctypes);

    do
    {
        vogl_trace_file_reader::trace_file_reader_status_t read_status = pTrace_reader->read_next_packet();

        if ((read_status != vogl_trace_file_reader::cOK) && (read_status != vogl_trace_file_reader::cEOF))
        {
            vogl_error_printf("%s: Failed reading from keyframe trace file!\n", VOGL_FUNCTION_INFO_CSTR);
            return NULL;
        }

        if ((read_status == vogl_trace_file_reader::cEOF) || (pTrace_reader->get_packet_type() == cTSPTEOF))
        {
            vogl_error_printf("%s: Failed finding state snapshot in keyframe file!\n", VOGL_FUNCTION_INFO_CSTR);
            return NULL;
        }

        if (pTrace_reader->get_packet_type() != cTSPTGLEntrypoint)
            continue;

        if (!keyframe_trace_packet.deserialize(pTrace_reader->get_packet_buf().get_ptr(), pTrace_reader->get_packet_buf().size(), false))
        {
            vogl_error_printf("%s: Failed parsing GL entrypoint packet in keyframe file\n", VOGL_FUNCTION_INFO_CSTR);
            return NULL;
        }

        const vogl_trace_gl_entrypoint_packet *pGL_packet = &pTrace_reader->get_packet<vogl_trace_gl_entrypoint_packet>();
        gl_entrypoint_id_t entrypoint_id = static_cast<gl_entrypoint_id_t>(pGL_packet->m_entrypoint_id);

        //const gl_entrypoint_desc_t &entrypoint_desc = g_vogl_entrypoint_descs[entrypoint_id];

        if (vogl_is_swap_buffers_entrypoint(entrypoint_id) || vogl_is_draw_entrypoint(entrypoint_id) || vogl_is_make_current_entrypoint(entrypoint_id))
        {
            vogl_error_printf("Failed finding state snapshot in keyframe file!\n");
            return NULL;
        }

        if (entrypoint_id == VOGL_ENTRYPOINT_glInternalTraceCommandRAD)
        {
            GLuint cmd = keyframe_trace_packet.get_param_value<GLuint>(0);
            GLuint size = keyframe_trace_packet.get_param_value<GLuint>(1);
            VOGL_NOTE_UNUSED(size);

            if (cmd == cITCRKeyValueMap)
            {
                key_value_map &kvm = keyframe_trace_packet.get_key_value_map();

                dynamic_string cmd_type(kvm.get_string("command_type"));
                if (cmd_type == "state_snapshot")
                {
                    dynamic_string id(kvm.get_string("binary_id"));
                    if (id.is_empty())
                    {
                        vogl_error_printf("%s: Missing binary_id field in glInternalTraceCommandRAD key_value_map command type: \"%s\"\n", VOGL_FUNCTION_INFO_CSTR, cmd_type.get_ptr());
                        return NULL;
                    }

                    uint8_vec snapshot_data;
                    {
                        timed_scope ts2("get_multi_blob_manager().get");
                        if (!pTrace_reader->get_multi_blob_manager().get(id, snapshot_data) || (snapshot_data.is_empty()))
                        {
                            vogl_error_printf("%s: Failed reading snapshot blob data \"%s\"!\n", VOGL_FUNCTION_INFO_CSTR, id.get_ptr());
                            return NULL;
                        }
                    }

                    vogl_message_printf("%s: Deserializing state snapshot \"%s\", %u bytes\n", VOGL_FUNCTION_INFO_CSTR, id.get_ptr(), snapshot_data.size());

                    json_document doc;
                    {
                        timed_scope ts2("doc.binary_deserialize");
                        if (!doc.binary_deserialize(snapshot_data) || (!doc.get_root()))
                        {
                            vogl_error_printf("%s: Failed deserializing JSON snapshot blob data \"%s\"!\n", VOGL_FUNCTION_INFO_CSTR, id.get_ptr());
                            return NULL;
                        }
                    }

                    pSnapshot = vogl_new(vogl_gl_state_snapshot);

                    timed_scope ts2("pSnapshot->deserialize");
                    if (!pSnapshot->deserialize(*doc.get_root(), pTrace_reader->get_multi_blob_manager(), &trace_gl_ctypes))
                    {
                        vogl_delete(pSnapshot);
                        pSnapshot = NULL;

                        vogl_error_printf("%s: Failed deserializing snapshot blob data \"%s\"!\n", VOGL_FUNCTION_INFO_CSTR, id.get_ptr());
                        return NULL;
                    }

                    found_snapshot = true;
                }
            }
        }

    } while (!found_snapshot);

    return pSnapshot;
}

//----------------------------------------------------------------------------------------------------------------------
// get_replayer_flags_from_command_line_params
//----------------------------------------------------------------------------------------------------------------------
static uint32_t get_replayer_flags_from_command_line_params(bool interactive_mode)
{
    uint32_t replayer_flags = 0;

    static struct
    {
        const char *m_pCommand;
        uint32_t m_flag;
    } s_replayer_command_line_params[] =
    {
        { "benchmark", cGLReplayerBenchmarkMode },
        { "verbose", cGLReplayerVerboseMode },
        { "force_debug_context", cGLReplayerForceDebugContexts },
        { "dump_all_packets", cGLReplayerDumpAllPackets },
        { "debug", cGLReplayerDebugMode },
        { "lock_window_dimensions", cGLReplayerLockWindowDimensions },
        { "replay_debug", cGLReplayerLowLevelDebugMode },
        { "dump_packet_blob_files_on_error", cGLReplayerDumpPacketBlobFilesOnError },
        { "dump_shaders_on_draw", cGLReplayerDumpShadersOnDraw },
        { "dump_packets_on_error", cGLReplayerDumpPacketsOnError },
        { "dump_screenshots", cGLReplayerDumpScreenshots },
        { "hash_backbuffer", cGLReplayerHashBackbuffer },
        { "dump_backbuffer_hashes", cGLReplayerDumpBackbufferHashes },
        { "sum_hashing", cGLReplayerSumHashing },
        { "dump_framebuffer_on_draw", cGLReplayerDumpFramebufferOnDraws },
        { "clear_uninitialized_bufs", cGLReplayerClearUnintializedBuffers },
        { "disable_frontbuffer_restore", cGLReplayerDisableRestoreFrontBuffer },
    };

    for (uint32_t i = 0; i < sizeof(s_replayer_command_line_params) / sizeof(s_replayer_command_line_params[0]); i++)
        if (g_command_line_params().get_value_as_bool(s_replayer_command_line_params[i].m_pCommand))
            replayer_flags |= s_replayer_command_line_params[i].m_flag;

    if (interactive_mode && !g_command_line_params().get_value_as_bool("disable_snapshot_caching"))
        replayer_flags |= cGLReplayerSnapshotCaching;

    return replayer_flags;
}

//----------------------------------------------------------------------------------------------------------------------
// check_events
//----------------------------------------------------------------------------------------------------------------------
static int check_events(replay_data_t &rdata)
{
    vogl::hash_map<uint64_t> &keys_down = rdata.keys_down;
    vogl::hash_map<uint64_t> &keys_pressed = rdata.keys_pressed;

    while (X11_Pending(rdata.window.get_display()))
    {
        XEvent newEvent;

        // Watch for new X eventsn
        XNextEvent(rdata.window.get_display(), &newEvent);

        switch (newEvent.type)
        {
        case KeyPress:
        {
            KeySym xsym = XLookupKeysym(&newEvent.xkey, 0);

            //printf("KeyPress 0%04" PRIX64 "%" PRIu64 "\n", (uint64_t)xsym, (uint64_t)xsym);

            keys_down.insert(xsym);
            keys_pressed.insert(xsym);
            break;
        }
        case KeyRelease:
        {
            KeySym xsym = XLookupKeysym(&newEvent.xkey, 0);

            //printf("KeyRelease 0x%04" PRIX64 " %" PRIu64 "\n", (uint64_t)xsym, (uint64_t)xsym);
            keys_down.erase(xsym);
            break;
        }
        case FocusIn:
        case FocusOut:
        {
            //printf("FocusIn/FocusOut\n");
            keys_down.reset();
            break;
        }
        case MappingNotify:
        {
            //XRefreshKeyboardMapping(&newEvent);
            break;
        }
        case UnmapNotify:
        {
            //printf("UnmapNotify\n");
            rdata.win_mapped = false;
            keys_down.reset();
            break;
        }
        case MapNotify:
        {
            //printf("MapNotify\n");

            rdata.win_mapped = true;

            keys_down.reset();

            if (!rdata.replayer.update_window_dimensions())
                return -1;
            break;
        }
        case ConfigureNotify:
        {
            if (!rdata.replayer.update_window_dimensions())
                return -1;
            break;
        }
        case DestroyNotify:
        {
            vogl_message_printf("Exiting\n");
            return 1;
        }
        case ClientMessage:
        {
            if (newEvent.xclient.data.l[0] == (int)rdata.wmDeleteMessage)
            {
                vogl_message_printf("Exiting\n");
                return 1;
            }
            break;
        }
        default:
            break;
        }
    }
    
    return 0;
}

//----------------------------------------------------------------------------------------------------------------------
// do_interactive_mode
//----------------------------------------------------------------------------------------------------------------------
static int do_interactive_mode(replay_data_t &rdata)
{
    if (!rdata.win_mapped)
    {
        vogl_sleep(10);
        return 0;
    }

    tmZone(TELEMETRY_LEVEL0, TMZF_NONE, "Interactive");

    vogl_gl_replayer &replayer = rdata.replayer;
    vogl::hash_map<uint64_t> &keys_down = rdata.keys_down;
    vogl::hash_map<uint64_t> &keys_pressed = rdata.keys_pressed;
    
    // Interactive mode is more of a test bad to validate a bunch of classes. It's kind of narly because the replayer's object can be in odd intermediate/pending states during window
    // resizes - hopefully this complexity will go away once we get pbuffer's or whatever in.
    vogl_debug_printf("%s: At frame boundary: %u, beginning of frame %u, pause frame %" PRIi64 ", taking snapshot at frame %" PRIi64 "\n",
                      VOGL_FUNCTION_INFO_CSTR, replayer.get_at_frame_boundary(),
                      replayer.get_frame_index(), rdata.paused_mode_frame_index,
                      rdata.take_snapshot_at_frame_index);

    if (keys_pressed.contains('c'))
    {
        keys_pressed.erase('c');
        if (replayer.is_valid())
        {
            dynamic_string filename;
            for (uint32_t i = 0; i < 10000000; i++)
            {
                filename.format("screenshot_%06u.png", i);
                if (!file_utils::does_file_exist(filename.get_ptr()))
                    break;
            }

            replayer.dump_frontbuffer_screenshot_before_next_swap(filename);
        }
    }

    if (keys_pressed.contains('s'))
    {
        keys_pressed.erase('s');
        rdata.slow_mode = !rdata.slow_mode;
    }

    // When paused, we'll NOT be at a frame boundary because the prev. loop applied a state snapshot (which will be pending)
    if (replayer.get_at_frame_boundary() && !replayer.get_pending_apply_snapshot())
    {
        bool take_new_snapshot = false;

        // See if we've scheduled a snapshot at this frame
        if ((int64_t)replayer.get_frame_index() == rdata.take_snapshot_at_frame_index)
        {
            take_new_snapshot = true;

            rdata.take_snapshot_at_frame_index = -1;
        }
        // Check for pausing
        else if (keys_pressed.contains(XK_space))
        {
            keys_pressed.erase(XK_space);

            if (rdata.paused_mode)
            {
                console::info("Unpausing\n");

                keys_pressed.erase(XK_space);

                vogl_delete(rdata.pSnapshot);
                rdata.pSnapshot = NULL;

                rdata.paused_mode_frame_index = -1;
                rdata.paused_mode = false;
            }
            else
            {
                console::info("Pausing\n");

                rdata.paused_mode = true;
                take_new_snapshot = true;
            }
        }

        // Snapshot the current state
        if (take_new_snapshot)
        {
            vogl_delete(rdata.pSnapshot);
            rdata.pSnapshot = NULL;

            rdata.pSnapshot = replayer.snapshot_state();
            if (!rdata.pSnapshot)
            {
                vogl_error_printf("%s: Snapshot failed!\n", VOGL_FUNCTION_INFO_CSTR);
                return -1;
            }

            if (g_command_line_params().get_value_as_bool("debug_test_snapshot_serialization"))
            {
                // Obviously, this crap is only for debugging.
                vogl_memory_blob_manager mem_blob_manager;
                mem_blob_manager.init(cBMFReadWrite);

                json_document temp_json_doc;
                if (!rdata.pSnapshot->serialize(*temp_json_doc.get_root(), mem_blob_manager, &replayer.get_trace_gl_ctypes()))
                {
                    console::error("%s: Failed serializing state snapshot!\n", VOGL_FUNCTION_INFO_CSTR);
                }
                else
                {
                    uint8_vec ubj_data;
                    temp_json_doc.binary_serialize(ubj_data);
                    temp_json_doc.binary_deserialize(ubj_data);
                    ubj_data.clear();

                    vogl_gl_state_snapshot *pNew_snapshot = vogl_new(vogl_gl_state_snapshot);

                    if (!pNew_snapshot->deserialize(*temp_json_doc.get_root(), mem_blob_manager, &replayer.get_trace_gl_ctypes()))
                    {
                        console::error("%s: Failed deserializing state snapshot!\n", VOGL_FUNCTION_INFO_CSTR);
                    }
                    else
                    {
                        vogl_delete(rdata.pSnapshot);

                        rdata.pSnapshot = pNew_snapshot;
                    }
                }
            }

            rdata.paused_mode_frame_index = replayer.get_frame_index();
            rdata.paused_mode = true;
        }
    }

    // Begin processing the next frame
    bool applied_snapshot = false;
    vogl_gl_replayer::status_t status = replayer.process_pending_window_resize(&applied_snapshot);

    if (status == vogl_gl_replayer::cStatusOK)
    {
        if (replayer.get_at_frame_boundary())
        {
            const char *pWindow_name = (sizeof(void *) == sizeof(uint32_t)) ? "voglreplay 32-bit" : "voglreplay 64-bit";
            dynamic_string window_title(cVarArg, "%s: File: %s Frame %u %s %s",
                                        pWindow_name,
                                        rdata.trace_filename.get_ptr(),
                                        replayer.get_frame_index(),
                                        rdata.paused_mode ? "PAUSED" : "",
                                        rdata.keyframes.find_sorted(replayer.get_frame_index()) >= 0 ? "(At Keyframe)" : "");
            rdata.window.set_title(window_title.get_ptr());
        }

        // At this point, if we're paused the frame snapshot as been applied, and we're just about going to replay the frame's commands.
        if (((applied_snapshot) || (replayer.get_at_frame_boundary())) &&
                (keys_pressed.contains('t') || keys_pressed.contains('j')))
        {
            uint64_t frame_to_trim;
            if (rdata.paused_mode)
                frame_to_trim = rdata.paused_mode_frame_index;
            else
                frame_to_trim = replayer.get_frame_index();

            dynamic_string trim_name;
            for (uint32_t i = 0; i < 1000000; i++)
            {
                trim_name.format("trim_%06" PRIu64 "_%u", frame_to_trim, i);
                if (!file_utils::does_dir_exist(trim_name.get_ptr()))
                    break;
            }

            if (!file_utils::create_directory(trim_name.get_ptr()))
            {
                vogl_error_printf("%s: Failed creating trim directory %s\n", VOGL_FUNCTION_INFO_CSTR, trim_name.get_ptr());
            }
            else
            {
                dynamic_string trim_filename(trim_name + "/" + trim_name + ".bin");
                dynamic_string snapshot_id;
                uint32_t write_trim_file_flags = vogl_gl_replayer::cWriteTrimFileFromStartOfFrame | (g_command_line_params().get_value_as_bool("no_trim_optimization") ? 0 : vogl_gl_replayer::cWriteTrimFileOptimizeSnapshot);
                if (rdata.replayer.write_trim_file(write_trim_file_flags, trim_filename, 1, *rdata.pTrace_reader, &snapshot_id))
                {
                    dynamic_string json_trim_base_filename(trim_name + "/j" + trim_name);

                    char voglreplay_exec_filename[1024];
                    file_utils::get_exec_filename(voglreplay_exec_filename, sizeof(voglreplay_exec_filename));

                    dynamic_string convert_to_json_spawn_str(cVarArg, "%s --dump \"%s\" \"%s\"", voglreplay_exec_filename, trim_filename.get_ptr(), json_trim_base_filename.get_ptr());
                    if (system(convert_to_json_spawn_str.get_ptr()) != 0)
                    {
                        vogl_error_printf("%s: Failed running voglreplay: %s\n", VOGL_FUNCTION_INFO_CSTR, convert_to_json_spawn_str.get_ptr());
                    }
                    else
                    {
                        dynamic_string json_trim_full_filename(trim_name + "/j" + trim_name + "_000000.json");

                        dynamic_string view_json_spawn_str(cVarArg, "np \"%s\" &", json_trim_full_filename.get_ptr());
                        system(view_json_spawn_str.get_ptr());
                    }

                    if (keys_pressed.contains('j'))
                    {
                        dynamic_string workdir(".");
                        file_utils::full_path(workdir);

                        dynamic_string replay_json_spawn_str(cVarArg, "konsole --workdir \"%s\" --hold -e \"%s\" --endless \"%s\" &", workdir.get_ptr(), voglreplay_exec_filename, json_trim_base_filename.get_ptr());
                        system(replay_json_spawn_str.get_ptr());
                    }
                }
            }

            keys_pressed.erase('t');
            keys_pressed.erase('j');
        }

        // Now replay the next frame's GL commands up to the swap
        status = replayer.process_frame(*rdata.pTrace_reader);
    }

    if (status == vogl_gl_replayer::cStatusHardFailure)
        return -1;

    if ((rdata.slow_mode) && (!rdata.paused_mode))
        vogl_sleep(100);

    int64_t seek_to_target_frame = -1;
    bool seek_to_closest_keyframe = false;
    int seek_to_closest_frame_dir_bias = 0;

    if (status == vogl_gl_replayer::cStatusAtEOF)
    {
        vogl_message_printf("%s: At trace EOF, frame index %u\n", VOGL_FUNCTION_INFO_CSTR, replayer.get_frame_index());

        // Right after the last swap in the file, either rewind or pause back on the last frame
        if ((rdata.paused_mode) && (replayer.get_frame_index()))
        {
            seek_to_target_frame = replayer.get_frame_index() - 1;
            rdata.take_snapshot_at_frame_index = -1;
        }
        else
        {
            vogl_printf("Resetting state and rewinding back to frame 0\n");

            replayer.reset_state();

            if (!rdata.pTrace_reader->seek_to_frame(0))
            {
                vogl_error_printf("%s: Failed rewinding trace reader!\n", VOGL_FUNCTION_INFO_CSTR);
                return -1;
            }

            rdata.take_snapshot_at_frame_index = -1;
            rdata.paused_mode_frame_index = -1;
            rdata.paused_mode = false;
        }

        vogl_delete(rdata.pSnapshot);
        rdata.pSnapshot = NULL;
    }
    else if (replayer.get_at_frame_boundary() && (!replayer.get_pending_apply_snapshot()))
    {
        // Rewind to beginning
        if (keys_pressed.contains('r'))
        {
            bool ctrl = (keys_down.contains(XK_Control_L) || keys_down.contains(XK_Control_R));
            keys_pressed.erase('r');

            vogl_delete(rdata.pSnapshot);
            rdata.pSnapshot = NULL;

            if ((rdata.paused_mode) && (ctrl))
                seek_to_target_frame = rdata.paused_mode_frame_index;

            rdata.take_snapshot_at_frame_index = -1;
            rdata.paused_mode_frame_index = -1;
            rdata.paused_mode = false;

            vogl_printf("Resetting state and rewinding back to frame 0\n");

            replayer.reset_state();

            if (!rdata.pTrace_reader->seek_to_frame(0))
            {
                vogl_error_printf("%s: Failed rewinding trace reader!\n", VOGL_FUNCTION_INFO_CSTR);
                return -1;
            }
        }
        // Seek to last frame
        else if (keys_pressed.contains('e'))
        {
            keys_pressed.erase('e');

            if (rdata.paused_mode)
            {
                vogl_delete(rdata.pSnapshot);
                rdata.pSnapshot = NULL;

                rdata.paused_mode_frame_index = -1;
                rdata.take_snapshot_at_frame_index = -1;

                vogl_printf("Seeking to last frame\n");

                int64_t max_frame_index = rdata.pTrace_reader->get_max_frame_index();
                if (max_frame_index < 0)
                {
                    vogl_error_printf("%s: Failed determining the total number of trace frames!\n", VOGL_FUNCTION_INFO_CSTR);
                    return -1;
                }

                // "frames" are actually "swaps" to the tracer/replayer, so don't seek to the "last" swap (because no rendering will follow that one), but the one before
                seek_to_target_frame = max_frame_index - 1;
            }
        }
        // Check if paused and process seek related keypresses
        else if ((rdata.paused_mode) && (rdata.pSnapshot))
        {
            int num_key;
            for (num_key = '0'; num_key <= '9'; num_key++)
            {
                if (keys_pressed.contains(num_key))
                {
                    keys_pressed.erase(num_key);
                    break;
                }
            }

            if (num_key <= '9')
            {
                int64_t max_frame_index = rdata.pTrace_reader->get_max_frame_index();
                if (max_frame_index < 0)
                {
                    vogl_error_printf("%s: Failed determining the total number of trace frames!\n", VOGL_FUNCTION_INFO_CSTR);
                    return -1;
                }

                float fraction = ((num_key - '0') + 1) / 11.0f;

                seek_to_target_frame = math::clamp<int64_t>(static_cast<int64_t>(max_frame_index * fraction + .5f), 0, max_frame_index - 1);
                seek_to_closest_keyframe = true;
            }
            else if (keys_pressed.contains(XK_Left) || keys_pressed.contains(XK_Right))
            {
                int dir = keys_pressed.contains(XK_Left) ? -1 : 1;

                bool shift = (keys_down.contains(XK_Shift_L) || keys_down.contains(XK_Shift_R));
                bool ctrl = (keys_down.contains(XK_Control_L) || keys_down.contains(XK_Control_R));

                int mag = 1;
                if ((shift) && (ctrl))
                    mag = 500;
                else if (shift)
                    mag = 10;
                else if (ctrl)
                    mag = 100;

                int rel = dir * mag;

                int64_t target_frame_index = math::maximum<int64_t>(0, rdata.paused_mode_frame_index + rel);

                seek_to_target_frame = target_frame_index;

                keys_pressed.erase(XK_Left);
                keys_pressed.erase(XK_Right);

                if ((rdata.keyframes.size()) && (keys_down.contains(XK_Alt_L) || keys_down.contains(XK_Alt_R)))
                {
                    uint32_t keyframe_array_index = 0;
                    for (keyframe_array_index = 1; keyframe_array_index < rdata.keyframes.size(); keyframe_array_index++)
                        if ((int64_t)rdata.keyframes[keyframe_array_index] > rdata.paused_mode_frame_index)
                            break;

                    if (dir < 0)
                    {
                        if ((rdata.paused_mode_frame_index == static_cast<int64_t>(rdata.keyframes[keyframe_array_index - 1])) && (keyframe_array_index > 1))
                            keyframe_array_index = keyframe_array_index - 2;
                        else
                            keyframe_array_index = keyframe_array_index - 1;
                    }
                    else
                    {
                        if (keyframe_array_index < rdata.keyframes.size())
                        {
                            if ((rdata.paused_mode_frame_index == static_cast<int64_t>(rdata.keyframes[keyframe_array_index])) && ((keyframe_array_index + 1) < rdata.keyframes.size()))
                                keyframe_array_index = keyframe_array_index + 1;
                            //else
                            //   keyframe_array_index = keyframe_array_index;
                        }
                        else
                            keyframe_array_index = keyframe_array_index - 1;
                    }

                    seek_to_target_frame = rdata.keyframes[keyframe_array_index];

                    if (mag > 1)
                    {
                        if (((dir < 0) && (target_frame_index < seek_to_target_frame)) || (target_frame_index > seek_to_target_frame))
                            seek_to_target_frame = target_frame_index;

                        seek_to_closest_keyframe = true;
                        seek_to_closest_frame_dir_bias = dir;
                    }

                    console::info("Seeking to keyframe array index %u, target frame %" PRIu64 "\n", keyframe_array_index, seek_to_target_frame);
                }
            }
            // Check for unpause
            else if (keys_pressed.contains(XK_space))
            {
                console::info("Unpausing\n");

                keys_pressed.erase(XK_space);

                vogl_delete(rdata.pSnapshot);
                rdata.pSnapshot = NULL;

                rdata.paused_mode_frame_index = -1;
                rdata.paused_mode = false;
            }
            else
            {
                // We're paused so seek back to the command right after the swap we're paused on, apply the paused frame's snapshot, then play the frame over
                status = replayer.begin_applying_snapshot(rdata.pSnapshot, false);
                if ((status != vogl_gl_replayer::cStatusOK) && (status != vogl_gl_replayer::cStatusResizeWindow))
                {
                    vogl_error_printf("%s: Failed applying snapshot!\n", VOGL_FUNCTION_INFO_CSTR);
                    return -1;
                }

                rdata.pTrace_reader->seek_to_frame(static_cast<uint32_t>(rdata.paused_mode_frame_index));
            }
        }
    }

    // Seek to target frame
    if (seek_to_target_frame != -1)
    {
        vogl_delete(rdata.pSnapshot);
        rdata.pSnapshot = NULL;
        rdata.paused_mode_frame_index = -1;

        if ((int64_t)replayer.get_frame_index() == seek_to_target_frame)
            rdata.take_snapshot_at_frame_index = seek_to_target_frame;
        else
        {
            uint32_t keyframe_array_index = 0;
            for (keyframe_array_index = 1; keyframe_array_index < rdata.keyframes.size(); keyframe_array_index++)
                if (static_cast<int64_t>(rdata.keyframes[keyframe_array_index]) > seek_to_target_frame)
                    break;

            if ((!rdata.keyframes.is_empty()) && (static_cast<int64_t>(rdata.keyframes[keyframe_array_index - 1]) <= seek_to_target_frame))
            {
                int keyframe_array_index_to_use = keyframe_array_index - 1;

                if (seek_to_closest_keyframe)
                {
                    if (!seek_to_closest_frame_dir_bias)
                    {
                        if ((keyframe_array_index_to_use + 1U) < rdata.keyframes.size())
                        {
                            if (llabs(static_cast<int64_t>(rdata.keyframes[keyframe_array_index_to_use + 1]) - seek_to_target_frame) < llabs(static_cast<int64_t>(rdata.keyframes[keyframe_array_index_to_use]) - seek_to_target_frame))
                            {
                                keyframe_array_index_to_use++;
                            }
                        }
                    }
                    else if (seek_to_closest_frame_dir_bias > 0)
                    {
                        if ((keyframe_array_index_to_use + 1U) < rdata.keyframes.size())
                        {
                            if (static_cast<int64_t>(rdata.keyframes[keyframe_array_index_to_use]) <= seek_to_target_frame)
                            {
                                keyframe_array_index_to_use++;
                            }
                        }
                    }
                }

                int64_t keyframe_index = rdata.keyframes[keyframe_array_index_to_use];

                if (seek_to_closest_keyframe)
                    seek_to_target_frame = keyframe_index;

                vogl_debug_printf("Seeking to target frame %" PRIu64 "\n", seek_to_target_frame);

                dynamic_string keyframe_filename(cVarArg, "%s_%06" PRIu64 ".bin", rdata.keyframe_base_filename.get_ptr(), keyframe_index);

                vogl_gl_state_snapshot *pKeyframe_snapshot = read_state_snapshot_from_trace(keyframe_filename);
                if (!pKeyframe_snapshot)
                    return -1;

                bool delete_snapshot_after_applying = true;
                if (seek_to_target_frame == keyframe_index)
                    delete_snapshot_after_applying = false;

                status = replayer.begin_applying_snapshot(pKeyframe_snapshot, delete_snapshot_after_applying);
                if ((status != vogl_gl_replayer::cStatusOK) && (status != vogl_gl_replayer::cStatusResizeWindow))
                {
                    vogl_error_printf("%s: Failed applying snapshot!\n", VOGL_FUNCTION_INFO_CSTR);
                    return -1;
                }

                pKeyframe_snapshot->set_frame_index(static_cast<uint32_t>(keyframe_index));

                if (!rdata.pTrace_reader->seek_to_frame(static_cast<uint32_t>(keyframe_index)))
                {
                    vogl_error_printf("%s: Failed seeking to keyframe!\n", VOGL_FUNCTION_INFO_CSTR);
                    return -1;
                }

                if (seek_to_target_frame == keyframe_index)
                {
                    rdata.pSnapshot = pKeyframe_snapshot;
                    rdata.paused_mode_frame_index = seek_to_target_frame;
                }
                else
                    rdata.take_snapshot_at_frame_index = seek_to_target_frame;
            }
            else
            {
                if (seek_to_target_frame < static_cast<int64_t>(replayer.get_frame_index()))
                {
                    replayer.reset_state();
                    rdata.pTrace_reader->seek_to_frame(0);
                }

                rdata.take_snapshot_at_frame_index = seek_to_target_frame;
            }
        }
    }

    return 0;
}

//----------------------------------------------------------------------------------------------------------------------
// do_non_interactive_mode
//----------------------------------------------------------------------------------------------------------------------
static int do_non_interactive_mode(replay_data_t &rdata)
{
    tmZone(TELEMETRY_LEVEL0, TMZF_NONE, "!Interactive");

    vogl_gl_replayer &replayer = rdata.replayer;
    vogl::vector<uint32_t> &trim_frames = rdata.trim_frames;

    if (replayer.get_at_frame_boundary())
    {
        if (trim_frames.size())
        {
            bool should_trim = false;
            uint32_t tf = 0;
            uint32_t len = 1;

            for (tf = 0; tf < trim_frames.size(); tf++)
            {
                if (rdata.trim_lens.size())
                {
                    if (rdata.trim_lens.size() < trim_frames.size())
                        len = rdata.trim_lens[0];
                    else
                        len = rdata.trim_lens[tf];
                }
                len = math::maximum(len, 1U);

                if (rdata.multitrim_mode)
                {
                    if ((replayer.get_frame_index() >= trim_frames[tf]) && (replayer.get_frame_index() < (trim_frames[tf] + math::maximum(len, 1U))))
                    {
                        should_trim = true;
                        break;
                    }
                }
                else
                {
                    if (replayer.get_frame_index() == trim_frames[tf])
                    {
                        should_trim = true;
                        break;
                    }
                }
            }

            if (rdata.multitrim_mode)
            {
                if (should_trim)
                {
                    if (rdata.multitrim_frames_remaining)
                    {
                        should_trim = false;
                    }
                    else
                    {
                        rdata.multitrim_frames_remaining = rdata.multitrim_interval;
                    }

                    rdata.multitrim_frames_remaining--;
                }
                else
                {
                    rdata.multitrim_frames_remaining = 0;
                }
            }
            //printf("multitrim_interval: %u %u\n", rdata.multitrim_frames_remaining, rdata.multitrim_interval);

            if (should_trim)
            {
                dynamic_string filename;

                if ((rdata.multitrim_mode) || (rdata.trim_filenames.size() < trim_frames.size()))
                {
                    filename = rdata.trim_filenames[0];

                    if ((rdata.multitrim_mode) || (trim_frames.size() > 1))
                    {
                        dynamic_string drive, dir, fname, ext;
                        file_utils::split_path(filename.get_ptr(), &drive, &dir, &fname, &ext);

                        dynamic_string new_fname(cVarArg, "%s_%06u", fname.get_ptr(), replayer.get_frame_index());

                        file_utils::combine_path_and_extension(filename, &drive, &dir, &new_fname, &ext);
                    }
                }
                else
                {
                    filename = rdata.trim_filenames[tf];
                }

                dynamic_string trim_path(file_utils::get_pathname(filename.get_ptr()));
                rdata.trim_file_blob_manager.set_path(trim_path);

                file_utils::create_directories(trim_path, false);

                uint32_t write_trim_file_flags = vogl_gl_replayer::cWriteTrimFileFromStartOfFrame | (g_command_line_params().get_value_as_bool("no_trim_optimization") ? 0 : vogl_gl_replayer::cWriteTrimFileOptimizeSnapshot);
                if (!replayer.write_trim_file(write_trim_file_flags, filename, rdata.multitrim_mode ? 1 : len, *rdata.pTrace_reader))
                    return -1;

                rdata.num_trim_files_written++;

                if (!rdata.multitrim_mode)
                {
                    if (rdata.num_trim_files_written == trim_frames.size())
                    {
                        vogl_message_printf("%s: All requested trim files written, stopping replay\n", VOGL_FUNCTION_INFO_CSTR);
                        return 1;
                    }
                }
            }

            if (rdata.multitrim_mode)
            {
                uint64_t next_frame_index = replayer.get_frame_index() + 1;

                if (next_frame_index > rdata.highest_frame_to_trim)
                {
                    vogl_message_printf("%s: No more frames to trim, stopping replay\n", VOGL_FUNCTION_INFO_CSTR);
                    return 1;
                }
            }
        }

        if ((!rdata.pSnapshot) && (rdata.loop_frame != -1) && (static_cast<int64_t>(replayer.get_frame_index()) == rdata.loop_frame))
        {
            vogl_debug_printf("%s: Capturing replayer state at start of frame %u\n", VOGL_FUNCTION_INFO_CSTR, replayer.get_frame_index());

            rdata.pSnapshot = replayer.snapshot_state();

            if (rdata.pSnapshot)
            {
                vogl_printf("Snapshot succeeded\n");

                rdata.snapshot_loop_start_frame = rdata.pTrace_reader->get_cur_frame();
                rdata.snapshot_loop_end_frame = rdata.pTrace_reader->get_cur_frame() + rdata.loop_len;

                if (rdata.draw_kill_max_thresh > 0)
                {
                    replayer.set_frame_draw_counter_kill_threshold(0);
                }

                vogl_debug_printf("%s: Loop start: %" PRIi64 " Loop end: %" PRIi64 "\n", VOGL_FUNCTION_INFO_CSTR, rdata.snapshot_loop_start_frame, rdata.snapshot_loop_end_frame);
            }
            else
            {
                vogl_error_printf("Snapshot failed!\n");

                rdata.loop_frame = -1;
            }
        }
    }

    vogl_gl_replayer::status_t status = replayer.process_pending_window_resize();
    if (status == vogl_gl_replayer::cStatusOK)
    {
        for (;;)
        {
            status = replayer.process_next_packet(*rdata.pTrace_reader);

            if ((status != vogl_gl_replayer::cStatusHardFailure) && (status != vogl_gl_replayer::cStatusAtEOF))
            {
                if ((rdata.write_snapshot_index >= 0) && (rdata.write_snapshot_index == replayer.get_last_processed_call_counter()))
                {
                    dynamic_string filename(rdata.write_snapshot_filename);

                    dynamic_string write_snapshot_path(file_utils::get_pathname(filename.get_ptr()));
                    rdata.trim_file_blob_manager.init(cBMFReadWrite, write_snapshot_path.get_ptr());

                    file_utils::create_directories(write_snapshot_path, false);

                    rdata.pSnapshot = replayer.snapshot_state();

                    if (rdata.pSnapshot)
                    {
                        vogl_printf("Snapshot succeeded at call counter %" PRIu64 "\n", replayer.get_last_processed_call_counter());

                        vogl_null_blob_manager null_blob_manager;
                        null_blob_manager.init(cBMFReadWrite);

                        json_document doc;
                        vogl_blob_manager *pBlob_manager = g_command_line_params().get_value_as_bool("write_snapshot_blobs") ?
                                    static_cast<vogl_blob_manager *>(&rdata.trim_file_blob_manager) :
                                    static_cast<vogl_blob_manager *>(&null_blob_manager);
                        if (!rdata.pSnapshot->serialize(*doc.get_root(), *pBlob_manager, &replayer.get_trace_gl_ctypes()))
                        {
                            vogl_error_printf("Failed serializing state snapshot document!\n");
                        }
                        else if (!doc.serialize_to_file(filename.get_ptr(), true))
                        {
                            vogl_error_printf("Failed writing state snapshot to file \"%s\"!\n", filename.get_ptr());
                        }
                        else
                        {
                            vogl_printf("Successfully wrote JSON snapshot to file \"%s\"\n", filename.get_ptr());
                        }

                        vogl_delete(rdata.pSnapshot);
                        rdata.pSnapshot = NULL;
                    }
                    else
                    {
                        vogl_error_printf("Snapshot failed!\n");
                    }

                    return 1;
                }
                else if ((rdata.trim_call_index >= 0) && (rdata.trim_call_index == replayer.get_last_processed_call_counter()))
                {
                    dynamic_string filename(rdata.trim_filenames[0]);

                    dynamic_string trim_path(file_utils::get_pathname(filename.get_ptr()));
                    rdata.trim_file_blob_manager.set_path(trim_path);

                    file_utils::create_directories(trim_path, false);

                    if (!replayer.write_trim_file(0, filename, rdata.trim_lens.size() ? rdata.trim_lens[0] : 1, *rdata.pTrace_reader, NULL))
                        return -1;

                    vogl_message_printf("%s: Trim file written, stopping replay\n", VOGL_FUNCTION_INFO_CSTR);
                    return 1;
                }
            }

            if ((status == vogl_gl_replayer::cStatusNextFrame) || (status == vogl_gl_replayer::cStatusResizeWindow) || (status == vogl_gl_replayer::cStatusAtEOF) || (status == vogl_gl_replayer::cStatusHardFailure))
                break;
        }
    }

    if (status == vogl_gl_replayer::cStatusHardFailure)
        return -1;

    if (status == vogl_gl_replayer::cStatusAtEOF)
    {
        vogl_message_printf("%s: At trace EOF, frame index %u\n", VOGL_FUNCTION_INFO_CSTR, replayer.get_frame_index());
    }

    if ((replayer.get_at_frame_boundary()) && (rdata.pSnapshot) && (rdata.loop_count > 0) && ((rdata.pTrace_reader->get_cur_frame() == rdata.snapshot_loop_end_frame) || (status == vogl_gl_replayer::cStatusAtEOF)))
    {
        status = replayer.begin_applying_snapshot(rdata.pSnapshot, false);
        if ((status != vogl_gl_replayer::cStatusOK) && (status != vogl_gl_replayer::cStatusResizeWindow))
            return -1;

        rdata.pTrace_reader->seek_to_frame(static_cast<uint32_t>(rdata.snapshot_loop_start_frame));

        if (rdata.draw_kill_max_thresh > 0)
        {
            int64_t thresh = replayer.get_frame_draw_counter_kill_threshold();
            thresh += 1;
            if (thresh >= rdata.draw_kill_max_thresh)
                thresh = 0;
            replayer.set_frame_draw_counter_kill_threshold(thresh);

            vogl_debug_printf("%s: Applying snapshot and seeking back to frame %" PRIi64 " draw kill thresh %" PRIu64 "\n", VOGL_FUNCTION_INFO_CSTR, rdata.snapshot_loop_start_frame, thresh);
        }
        else
            vogl_debug_printf("%s: Applying snapshot and seeking back to frame %" PRIi64 "\n", VOGL_FUNCTION_INFO_CSTR, rdata.snapshot_loop_start_frame);

        rdata.loop_count--;
    }
    else
    {
        bool print_progress = (status == vogl_gl_replayer::cStatusAtEOF) || ((replayer.get_at_frame_boundary()) && ((replayer.get_frame_index() % 100) == 0));
        if (print_progress)
        {
            if (rdata.pTrace_reader->get_type() == cBINARY_TRACE_FILE_READER)
            {
                vogl_binary_trace_file_reader &binary_trace_reader = *static_cast<vogl_binary_trace_file_reader *>(rdata.pTrace_reader.get());

                vogl_printf("Replay now at frame index %d, trace file offet %" PRIu64 ", GL call counter %" PRIu64 ", %3.2f%% percent complete\n",
                            replayer.get_frame_index(),
                            binary_trace_reader.get_cur_file_ofs(),
                            replayer.get_last_parsed_call_counter(),
                            binary_trace_reader.get_trace_file_size() ? (binary_trace_reader.get_cur_file_ofs() * 100.0f) / binary_trace_reader.get_trace_file_size() : 0);
            }
        }

        if (status == vogl_gl_replayer::cStatusAtEOF)
        {
            if (!rdata.endless_mode)
            {
                double time_since_start = rdata.tm.get_elapsed_secs();

                vogl_printf("%u total swaps, %.3f secs, %3.3f avg fps\n", replayer.get_total_swaps(), time_since_start, replayer.get_frame_index() / time_since_start);
                return 1;
            }

            if (!rdata.benchmark_mode ||
                (rdata.benchmark_mode && rdata.benchmark_mode_allow_state_teardown))
            {
                vogl_printf("Resetting state and rewinding back to frame 0\n");

                replayer.reset_state();
            }

            if (rdata.benchmark_mode && !rdata.benchmark_mode_allow_state_teardown)
            {
                // the user is in the benchmark mode, but does not want to allow state teardown / restore,
                // so now that the first frame is complete, tell the replayer to not allow state restoring
                replayer.set_allow_snapshot_restoring(false);
            }

            if (!rdata.pTrace_reader->seek_to_frame(0))
            {
                vogl_error_printf("%s: Failed rewinding trace reader!\n", VOGL_FUNCTION_INFO_CSTR);
                return -1;
            }
        }
    }

    return 0;
}

//----------------------------------------------------------------------------------------------------------------------
// tool_replay_mode
//----------------------------------------------------------------------------------------------------------------------
bool tool_replay_mode()
{
    VOGL_FUNC_TRACER

    replay_data_t rdata;

    rdata.trace_filename = g_command_line_params().get_value_as_string_or_empty("", 1);
    if (rdata.trace_filename.is_empty())
    {
        vogl_error_printf("%s: No trace file specified!\n", VOGL_FUNCTION_INFO_CSTR);
        return false;
    }

    dynamic_string actual_trace_filename;
    rdata.pTrace_reader.reset(vogl_open_trace_file(rdata.trace_filename, actual_trace_filename, g_command_line_params().get_value_as_string_or_empty("loose_file_path").get_ptr()));
    if (!rdata.pTrace_reader.get())
    {
        vogl_error_printf("%s: File not found, or unable to determine file type of trace file \"%s\"\n",
                          VOGL_FUNCTION_INFO_CSTR, rdata.trace_filename.get_ptr());
        return false;
    }

    vogl_printf("Reading trace file %s\n", actual_trace_filename.get_ptr());

    bool interactive_mode = g_command_line_params().get_value_as_bool("interactive");
    uint32_t replayer_flags = get_replayer_flags_from_command_line_params(interactive_mode);

    // TODO: This will create a window with default attributes, which seems fine for the majority of traces.
    // Unfortunately, some GL call streams *don't* want an alpha channel, or depth, or stencil etc. in the default framebuffer so this may become a problem.
    // Also, this design only supports a single window, which is going to be a problem with multiple window traces.
    if (!rdata.window.open(g_command_line_params().get_value_as_int("width", 0, 1024, 1, 65535),
                           g_command_line_params().get_value_as_int("height", 0, 768, 1, 65535),
                           g_command_line_params().get_value_as_int("msaa", 0, 0, 0, 65535)))
    {
        vogl_error_printf("%s: Failed initializing replay window\n", VOGL_FUNCTION_INFO_CSTR);
        return false;
    }

    if (!rdata.replayer.init(replayer_flags, &rdata.window, rdata.pTrace_reader->get_sof_packet(), rdata.pTrace_reader->get_multi_blob_manager()))
    {
        vogl_error_printf("%s: Failed initializing GL replayer\n", VOGL_FUNCTION_INFO_CSTR);
        return false;
    }

    if (replayer_flags & cGLReplayerBenchmarkMode)
    {
        // Also disable all glGetError() calls in vogl_utils.cpp.
        vogl_disable_gl_get_error();
    }

    rdata.replayer.set_swap_sleep_time(g_command_line_params().get_value_as_uint("swap_sleep"));
    rdata.replayer.set_dump_framebuffer_on_draw_prefix(g_command_line_params().get_value_as_string("dump_framebuffer_on_draw_prefix", 0, "screenshot"));
    rdata.replayer.set_screenshot_prefix(g_command_line_params().get_value_as_string("dump_screenshots_prefix", 0, "screenshot"));
    rdata.replayer.set_backbuffer_hash_filename(g_command_line_params().get_value_as_string_or_empty("dump_backbuffer_hashes"));
    rdata.replayer.set_dump_framebuffer_on_draw_frame_index(g_command_line_params().get_value_as_int("dump_framebuffer_on_draw_frame", 0, -1, 0, INT_MAX));
    rdata.replayer.set_dump_framebuffer_on_draw_first_gl_call_index(g_command_line_params().get_value_as_int("dump_framebuffer_on_draw_first_gl_call", 0, -1, 0, INT_MAX));
    rdata.replayer.set_dump_framebuffer_on_draw_last_gl_call_index(g_command_line_params().get_value_as_int("dump_framebuffer_on_draw_last_gl_call", 0, -1, 0, INT_MAX));

    XSelectInput(rdata.window.get_display(), rdata.window.get_xwindow(),
                 EnterWindowMask | LeaveWindowMask | ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | ExposureMask | FocusChangeMask | KeyPressMask | KeyReleaseMask |
                 PropertyChangeMask | StructureNotifyMask | KeymapStateMask);

    rdata.wmDeleteMessage = XInternAtom(rdata.window.get_display(), "WM_DELETE_WINDOW", False);
    XSetWMProtocols(rdata.window.get_display(), rdata.window.get_xwindow(), &rdata.wmDeleteMessage, 1);

    rdata.keyframe_base_filename = g_command_line_params().get_value_as_string("keyframe_base_filename");
    rdata.take_snapshot_at_frame_index = g_command_line_params().get_value_as_int64("pause_on_frame", 0, -1);

    if (!rdata.keyframe_base_filename.is_empty())
    {
        find_files finder;
        if (!finder.find((rdata.keyframe_base_filename + "*.bin").get_ptr()))
        {
            vogl_error_printf("Failed finding files: %s\n", rdata.keyframe_base_filename.get_ptr());
            return false;
        }

        for (uint32_t i = 0; i < finder.get_files().size(); i++)
        {
            dynamic_string base_name(finder.get_files()[i].m_name);
            dynamic_string ext(base_name);
            file_utils::get_extension(ext);
            if (ext != "bin")
                continue;

            file_utils::remove_extension(base_name);
            int underscore_ofs = base_name.find_right('_');
            if (underscore_ofs < 0)
                continue;

            dynamic_string frame_index_str(base_name.right(underscore_ofs + 1));
            if (frame_index_str.is_empty())
                continue;

            const char *pFrame_index = frame_index_str.get_ptr();

            uint64_t frame_index = 0;
            if (!string_ptr_to_uint64(pFrame_index, frame_index))
                continue;

            rdata.keyframes.push_back(frame_index);
            printf("Found keyframe file %s index %" PRIu64 "\n", finder.get_files()[i].m_fullname.get_ptr(), frame_index);
        }

        rdata.keyframes.sort();
    }

    rdata.loop_frame = g_command_line_params().get_value_as_int("loop_frame", 0, -1);
    rdata.loop_len = math::maximum<int>(g_command_line_params().get_value_as_int("loop_len", 0, 1), 1);
    rdata.loop_count = math::maximum<int>(g_command_line_params().get_value_as_int("loop_count", 0, cINT32_MAX), 1);
    rdata.draw_kill_max_thresh = g_command_line_params().get_value_as_int("draw_kill_max_thresh", 0, -1);
    rdata.endless_mode = g_command_line_params().get_value_as_bool("endless");
    rdata.benchmark_mode = g_command_line_params().get_value_as_bool("benchmark");
    rdata.benchmark_mode_allow_state_teardown = g_command_line_params().get_value_as_bool("allow_state_teardown");

    rdata.multitrim_mode = g_command_line_params().get_value_as_bool("multitrim");
    rdata.multitrim_interval = g_command_line_params().get_value_as_int("multitrim_interval", 0, 1, 1);
    rdata.multitrim_frames_remaining = 0;

    rdata.write_snapshot_index = g_command_line_params().get_value_as_int64("write_snapshot_call", 0, -1, 0);
    rdata.write_snapshot_filename = g_command_line_params().get_value_as_string("write_snapshot_file", 0, "state_snapshot.json");

    rdata.trim_call_index = g_command_line_params().get_value_as_int64("trim_call", 0, -1, 0);
    rdata.trim_frames.resize(g_command_line_params().get_count("trim_frame"));

    for (uint32_t i = 0; i < rdata.trim_frames.size(); i++)
    {
        bool parsed_successfully;
        rdata.trim_frames[i] = g_command_line_params().get_value_as_uint("trim_frame", i, 0, 0, cUINT32_MAX, 0, &parsed_successfully);
        if (!parsed_successfully)
        {
            vogl_error_printf("%s: Failed parsing -trim_frame at index %u\n", VOGL_FUNCTION_INFO_CSTR, i);
            return false;
        }
    }

    rdata.trim_filenames.resize(g_command_line_params().get_count("trim_file"));
    for (uint32_t i = 0; i < rdata.trim_filenames.size(); i++)
    {
        dynamic_string filename(g_command_line_params().get_value_as_string("trim_file", i));

        if (filename.is_empty())
        {
            vogl_error_printf("%s: Invalid trim filename\n", VOGL_FUNCTION_INFO_CSTR);
            return false;
        }

        rdata.trim_filenames[i] = filename;

        if (file_utils::add_default_extension(rdata.trim_filenames[i], ".bin"))
            vogl_message_printf("%s: Trim output filename \"%s\", didn't have an extension, appended \".bin\" to the filename: %s\n", VOGL_FUNCTION_INFO_CSTR, filename.get_ptr(), rdata.trim_filenames[i].get_ptr());
    }

    rdata.trim_lens.resize(g_command_line_params().get_count("trim_len"));
    for (uint32_t i = 0; i < rdata.trim_lens.size(); i++)
    {
        bool parsed_successfully;
        rdata.trim_lens[i] = g_command_line_params().get_value_as_uint("trim_len", i, 1, 0, cUINT32_MAX, 0, &parsed_successfully);
        if (!parsed_successfully)
        {
            vogl_error_printf("%s: Failed parsing -trim_len at index %u\n", VOGL_FUNCTION_INFO_CSTR, i);
            return false;
        }
    }

    if (rdata.trim_frames.size())
    {
        if (rdata.trim_filenames.is_empty())
        {
            console::error("%s: -trim_frame specified without specifying at least one -trim_file or -trim_call\n", VOGL_FUNCTION_INFO_CSTR);
            return false;
        }
    }

    if (rdata.write_snapshot_index >= 0)
    {
        if ((rdata.multitrim_mode) || (rdata.trim_frames.size()) || (rdata.trim_lens.size()) || (rdata.trim_call_index >= 0))
        {
            console::warning("%s: Can't write snapshot and trim at the same time, disabling trimming\n", VOGL_FUNCTION_INFO_CSTR);

            rdata.multitrim_mode = false;
            rdata.trim_frames.clear();
            rdata.trim_lens.clear();
            rdata.trim_call_index = -1;
        }

        if (rdata.draw_kill_max_thresh > 0)
        {
            console::warning("%s: Write snapshot mode is enabled, disabling -draw_kill_max_thresh\n", VOGL_FUNCTION_INFO_CSTR);
            rdata.draw_kill_max_thresh = -1;
        }

        if (rdata.endless_mode)
        {
            console::warning("%s: Write snapshot mode is enabled, disabling -endless\n", VOGL_FUNCTION_INFO_CSTR);
            rdata.endless_mode = false;
        }
    }
    else if ((rdata.trim_frames.size()) || (rdata.trim_call_index != -1))
    {
        if (rdata.trim_filenames.is_empty())
        {
            console::error("%s: Must also specify at least one -trim_file\n", VOGL_FUNCTION_INFO_CSTR);
            return false;
        }

        if (rdata.trim_call_index != -1)
        {
            if (rdata.trim_frames.size())
            {
                console::error("%s: Can't specify both -trim_call and -trim_frame\n", VOGL_FUNCTION_INFO_CSTR);
                return false;
            }

            if (rdata.multitrim_mode)
            {
                console::error("%s: Can't specify both -trim_call and -multitrim\n", VOGL_FUNCTION_INFO_CSTR);
                return false;
            }

            if (rdata.trim_filenames.size() > 1)
            {
                console::warning("%s: -trim_call specified but more than 1 -trim_file specified - ignoring all but first -trim_file's\n", VOGL_FUNCTION_INFO_CSTR);
                rdata.trim_filenames.resize(1);
            }
        }

        rdata.trim_file_blob_manager.init(cBMFWritable);

        if (rdata.trim_frames.size() > 1)
        {
            if ((rdata.trim_filenames.size() > 1) && (rdata.trim_filenames.size() != rdata.trim_frames.size()))
            {
                console::error("%s: More than 1 -trim_frame was specified, and more than 1 -trim_file was specified, but the number of -trim_file's must match the number of -trim_frame's (or only specify one -trim_file to use it as a base filename)\n", VOGL_FUNCTION_INFO_CSTR);
                return false;
            }

            if ((rdata.trim_lens.size() > 1) && (rdata.trim_lens.size() != rdata.trim_frames.size()))
            {
                console::error("%s: More than 1 -trim_frame was specified, and more than 1 -trim_len's was specified, but the number of -trim_len's must match the number of -trim_frame's (or only specify one -trim_len to use it for all trims)\n", VOGL_FUNCTION_INFO_CSTR);
                return false;
            }
        }

        if ((rdata.multitrim_mode) && (rdata.trim_filenames.size() > 1))
        {
            console::warning("%s: Only 1 filename needs to be specified in -multitrim mode\n", VOGL_FUNCTION_INFO_CSTR);
        }

        if (rdata.loop_frame != -1)
        {
            console::warning("%s: Trim is enabled, disabling -loop_frame\n", VOGL_FUNCTION_INFO_CSTR);
            rdata.loop_frame = -1;
            rdata.loop_len = 1;
            rdata.loop_count = 1;
        }

        if (rdata.draw_kill_max_thresh > 0)
        {
            console::warning("%s: Trim is enabled, disabling -draw_kill_max_thresh\n", VOGL_FUNCTION_INFO_CSTR);
            rdata.draw_kill_max_thresh = -1;
        }

        if (rdata.endless_mode)
        {
            console::warning("%s: Trim is enabled, disabling -endless\n", VOGL_FUNCTION_INFO_CSTR);
            rdata.endless_mode = false;
        }

        if (rdata.trim_call_index == -1)
        {
            for (uint32_t tf = 0; tf < rdata.trim_frames.size(); tf++)
            {
                uint32_t len = 1;
                if (rdata.trim_lens.size())
                {
                    if (rdata.trim_lens.size() < rdata.trim_frames.size())
                        len = rdata.trim_lens[0];
                    else
                        len = rdata.trim_lens[tf];
                }
                len = math::maximum(len, 1U);
                rdata.highest_frame_to_trim = math::maximum<uint32_t>(rdata.highest_frame_to_trim, rdata.trim_frames[tf] + len - 1);
            }
        }
    }
    else
    {
        if (rdata.trim_filenames.size())
            console::warning("%s: -trim_file was specified, but -trim_frame was not specified so ignoring\n", VOGL_FUNCTION_INFO_CSTR);
        if (rdata.trim_lens.size())
            console::warning("%s: -trim_len was specified, but -trim_frame was not specified so ignoring\n", VOGL_FUNCTION_INFO_CSTR);

        rdata.trim_filenames.clear();
        rdata.trim_lens.clear();
    }

    rdata.tm.start();

    int ret = 0;
    for (;;)
    {
        tmZone(TELEMETRY_LEVEL0, TMZF_NONE, "Main Loop");

        // Return is -1:error, 1:done, 0:continue.
        ret = check_events(rdata);
        if (ret)
            break;

        // Return is -1:error, 1:done, 0:continue.
        ret = interactive_mode ? do_interactive_mode(rdata) : do_non_interactive_mode(rdata);
        if (ret)
            break;

        telemetry_tick();
    }

    if (ret != -1)
    {
        if (rdata.trim_frames.size())
        {
            console::message("Wrote %u trim file(s)\n", rdata.num_trim_files_written);

            if (((rdata.multitrim_mode) && (!rdata.num_trim_files_written)) ||
                    ((!rdata.multitrim_mode) && (rdata.num_trim_files_written != rdata.trim_frames.size())))
                console::warning("Requested %u trim frames, but was only able to write %u trim frames (one or more -trim_frames must have been too large)\n", rdata.trim_frames.size(), rdata.num_trim_files_written);
        }

        if (g_command_line_params().get_value_as_bool("pause_on_exit") && (rdata.window.is_opened()))
        {
            vogl_printf("Press a key to continue.\n");

            for (;;)
            {
                if (vogl_kbhit())
                    break;

                bool exit_flag = false;
                while (!exit_flag && X11_Pending(rdata.window.get_display()))
                {
                    XEvent newEvent;
                    XNextEvent(rdata.window.get_display(), &newEvent);

                    switch (newEvent.type)
                    {
                    case KeyPress:
                    case DestroyNotify:
                    {
                        exit_flag = true;
                        break;
                    }
                    case ClientMessage:
                    {
                        if (newEvent.xclient.data.l[0] == (int)rdata.wmDeleteMessage)
                            exit_flag = true;
                        break;
                    }
                    default:
                        break;
                    }
                }
                if (exit_flag)
                    break;
                vogl_sleep(50);
            }
        }
    }

    return (ret != -1);
}

bool tool_play_mode()
{
    return tool_replay_mode();
}

#endif  // VOGL_PLATFORM_HAS_X11
