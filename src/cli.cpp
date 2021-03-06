/*
 * Copyright 2017, alex at staticlibs.net
 * Copyright 2018, myasnikov.mike at gmail.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* 
 * File:   cli.cpp
 * Author: alex
 *
 * Created on June 6, 2017, 6:31 PM
 */

#include <cstdlib>
#include <array>
#include <iostream>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>
#include <utility>

#include "staticlib/config.hpp"
#if defined(STATICLIB_LINUX)
#include <unistd.h>
#elif defined(STATICLIB_MAC)
extern char** environ;
#endif

#include "popt.h"
#include "utf8.h"

#include "staticlib/support.hpp"
#ifdef STATICLIB_WINDOWS
#include "staticlib/support/windows.hpp"
#endif // STATICLIB_WINDOWS
#include "staticlib/io.hpp"
#include "staticlib/json.hpp"
#include "staticlib/utils.hpp"
#include "staticlib/tinydir.hpp"
#include "staticlib/unzip.hpp"

#include "wilton/wiltoncall.h"
#include "wilton/wilton_signal.h"

#include "wilton/support/exception.hpp"
#include "wilton/support/misc.hpp"

#include "cli_options.hpp"
#include "ghc_init.hpp"
#include "jvm_engine.hpp"

#define WILTON_QUOTE(value) #value
#define WILTON_STR(value) WILTON_QUOTE(value)
#ifndef WILTON_DEFAULT_SCRIPT_ENGINE
#define WILTON_DEFAULT_SCRIPT_ENGINE quickjs
#endif // WILTON_DEFAULT_SCRIPT_ENGINE
#define WILTON_DEFAULT_SCRIPT_ENGINE_STR WILTON_STR(WILTON_DEFAULT_SCRIPT_ENGINE)
#ifndef WILTON_VERSION
#define WILTON_VERSION UNSPECIFIED
#endif // !WILTON_VERSION
#define WILTON_VERSION_STR WILTON_STR(WILTON_VERSION)

namespace { // anonymous

int find_launcher_args_end(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if ("--" ==  std::string(argv[i])) {
            return i;
        }
    }
    return argc;
}

sl::support::optional<sl::json::value> load_app_config(const std::string& appdir) {
    if (!appdir.empty()) {
        auto pconf = sl::tinydir::path(appdir + "conf/");
        if (pconf.exists()) {
            auto cfile = sl::tinydir::path(appdir + "conf/config.json");
            if (cfile.exists() && !cfile.is_directory()) {
                auto src = cfile.open_read();
                auto val = sl::json::load(src);
                return sl::support::make_optional(std::move(val));
            }
        }
    }
    return sl::support::optional<sl::json::value>();
}

std::string read_appname(const std::string& appdir) {
    auto conf = load_app_config(appdir);
    if (conf.has_value()) {
        auto& json = conf.value();
        return json["appname"].as_string_nonempty_or_throw("conf/config.json:appname");
    }
    return std::string();
}

std::tuple<std::string, std::string, std::string> find_startup_module(
        const std::string& opts_startup_module_name, const std::string& startjs_full,
        const std::string& appdir) {
    auto mod = std::string();
    if (!opts_startup_module_name.empty()) {
        mod = opts_startup_module_name;
    } else {
        // try to get appname
        mod = read_appname(appdir);
        if (mod.empty()) {
            // fallback to dir name
            mod = sl::utils::strip_parent_dir(appdir);
            while(sl::utils::ends_with(mod, "/")) {
                mod.pop_back();
            }
        }
    }
    auto script = sl::utils::strip_parent_dir(startjs_full);
    if (sl::utils::ends_with(script, ".js")) {
        script = script.substr(0, script.length() - 3);
    }
    auto script_id = mod + "/" +script;
    return std::make_tuple(mod, appdir, script_id);
}

char platform_delimiter(const std::string& arg) {
#ifdef STATICLIB_WINDOWS
    char delim = ';';
#else // !STATICLIB_WINDOWS
    char delim = ':';
#endif // STATICLIB_WINDOWS
    if (sl::utils::starts_with(arg, ";") ||
            sl::utils::starts_with(arg, ":")) {
        delim = arg.at(0);
    }
    return delim;
}

std::vector<sl::json::field> prepare_paths(const std::string& wilton_home,
        const std::string& binary_modules_paths, const std::string& startmod,
        const std::string& startmod_dir) {
    std::vector<sl::json::field> res;
    // startup module
    res.emplace_back(startmod, wilton::support::file_proto_prefix + startmod_dir);
    // binary modules
    auto binmods = sl::utils::split(binary_modules_paths, platform_delimiter(binary_modules_paths));
    for(auto& mod : binmods) {
        if (!sl::utils::ends_with(mod, wilton::support::binmod_postfix)) {
            throw wilton::support::exception(TRACEMSG("Invalid binary module path specified," +
                    " must be 'path/to/mymod.wlib', path: [" + mod + "]"));
        }
        auto modpath = sl::tinydir::path(mod);
        if (!(modpath.exists() && modpath.is_regular_file())) {
            throw wilton::support::exception(TRACEMSG("Binary module file not found," +
                    " path: [" + mod + "]"));
        }
        auto modfile = sl::utils::strip_parent_dir(mod);
        auto modsubname = modfile.substr(0, modfile.length() - wilton::support::binmod_postfix.length());
        auto modname = startmod + "/" + modsubname;
        auto modfullpath = sl::tinydir::full_path(mod);
        res.emplace_back(modname, wilton::support::zip_proto_prefix + modfullpath);
    }
    // vendor libs
    auto libdir = sl::tinydir::path(wilton_home + "/lib");
    if (libdir.exists()) {
        for (sl::tinydir::path& libpath : sl::tinydir::list_directory(libdir.filepath())) {
            if (libpath.is_directory()) {
                auto& modname = libpath.filename();
                res.emplace_back(modname, wilton::support::file_proto_prefix + libpath.filepath());
            } else if (sl::utils::ends_with(libpath.filename(), ".js")) {
                const auto& modname = libpath.filename().substr(0, libpath.filename().length() - 3);
                const auto& dirpath = sl::utils::strip_filename(libpath.filepath());
                res.emplace_back(modname, wilton::support::file_proto_prefix + dirpath + modname);
            } else if (sl::utils::ends_with(libpath.filename(), ".wlib")) {
                const auto& modname = libpath.filename().substr(0, libpath.filename().length() - 5);
                res.emplace_back(modname, wilton::support::zip_proto_prefix + libpath.filepath());
            }
        }
    }
    return res;
}

void dyload_module(const std::string& name) {
    auto err_dyload = wilton_dyload(name.c_str(), static_cast<int>(name.length()), nullptr, 0);
    if (nullptr != err_dyload) {
        wilton::support::throw_wilton_error(err_dyload, TRACEMSG(err_dyload));
    }
}

void init_signals() {
    dyload_module("wilton_signal");
    auto err_init = wilton_signal_initialize();
    if (nullptr != err_init) {
        auto msg = TRACEMSG(err_init);
        wilton_free(err_init);
        throw wilton::support::exception(msg);
    }
#ifdef STATICLIB_WINDOWS
    // https://stackoverflow.com/a/9719240/314015
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif // STATICLIB_WINDOWS
}

sl::json::value read_json_file(const std::string& url) {
    auto path = url.substr(wilton::support::file_proto_prefix.length());
    auto src = sl::tinydir::file_source(path);
    return sl::json::load(src);
}

sl::json::value read_json_zip_entry(const std::string& zip_url, const std::string& entry) {
    dyload_module("wilton_zip");
    auto zip_path = zip_url.substr(wilton::support::zip_proto_prefix.length());
    auto idx = sl::unzip::file_index(zip_path);
    sl::unzip::file_entry en = idx.find_zip_entry(entry);
    if (en.is_empty()) throw wilton::support::exception(TRACEMSG(
            "Unable to load 'wilton-packages.json', ZIP entry: [" + entry + "]," +
            " file: [" + zip_path + "]"));
    auto stream = sl::unzip::open_zip_entry(idx, entry);
    auto src = sl::io::streambuf_source(stream->rdbuf());
    return sl::json::load(src);
}

std::vector<sl::json::value> load_packages_list(const std::string& modurl) {
    // note: cannot use 'wilton_loader' here, as it is not initialized yet
    auto packages_json_id = "wilton-requirejs/wilton-packages.json";
    auto res = sl::json::value();
    if (sl::utils::starts_with(modurl, wilton::support::zip_proto_prefix)) {
        res = read_json_zip_entry(modurl, packages_json_id);
    } else if (sl::utils::starts_with(modurl, wilton::support::file_proto_prefix)) {
        res = read_json_file(modurl + packages_json_id);
    } else {
        throw wilton::support::exception(TRACEMSG(
                "Unable to load 'wilton-packages.json' - unknown protocol," +
                " baseUrl: [" + modurl + "]"));
    }
    return std::move(res.as_array_or_throw(packages_json_id));
}

std::string get_env_var_value(const std::vector<std::string>& parts) {
    if (parts.size() < 2) throw wilton::support::exception(TRACEMSG(
            "Invalid environment variable vector specified," +
            " parts count: [" + sl::support::to_string(parts.size()) + "]"));
    auto value = sl::utils::trim(parts.at(1));
    for (size_t i = 2; i < parts.size(); i++) {
        value.push_back('=');
        value += sl::utils::trim(parts.at(i));
    }
    return value;
}

void set_env_vars(const std::string& environment_vars) {
    auto vars = sl::utils::split(environment_vars, platform_delimiter(environment_vars));
    for (auto& var : vars) {
        auto parts = sl::utils::split(var, '=');
        if (parts.size() < 2) throw wilton::support::exception(TRACEMSG(
                    "Invalid environment variable specified," +
                    " must be in 'name=value' format, var: [" + var + "]"));
        auto name = sl::utils::trim(parts.at(0));
        auto value = get_env_var_value(parts);
#ifdef STATICLIB_WINDOWS
        auto err = _putenv_s(name.c_str(), value.c_str());
#else // !STATICLIB_WINDOWS
        auto err = setenv(name.c_str(), value.c_str(), 1);
#endif
        if (0 != err) throw wilton::support::exception(TRACEMSG(
                "Error setting environment variable,"
                " name: [" + name + "]," +
                " value: [" + value + "]"));
    }
}

std::vector<sl::json::field> collect_env_vars() {
#ifdef STATICLIB_WINDOWS
    auto envp = _environ;
#else // !STATICLIB_WINDOWS
    auto envp = environ;
#endif
    auto vec = std::vector<sl::json::field>();
    for (char** el = envp; *el != nullptr; el++) {
        auto var = std::string(*el);
        auto parts = sl::utils::split(var, '=');
        if (parts.size() >= 2) {
            auto name = sl::utils::trim(parts.at(0));
            auto name_clean = std::string();
            utf8::replace_invalid(name.begin(), name.end(), std::back_inserter(name_clean));
            auto value = get_env_var_value(parts);
            auto value_clean = std::string();
            utf8::replace_invalid(value.begin(), value.end(), std::back_inserter(value_clean));
            vec.emplace_back(std::move(name_clean), std::move(value_clean));
        }
    }
    std::sort(vec.begin(), vec.end(), [](const sl::json::field& a, const sl::json::field& b) {
        return a.name() < b.name();
    });
    return vec;
}

std::string write_temp_one_liner(const std::string& deps, const std::string& code) {
    // prepare tmp file path
    auto rsg = sl::utils::random_string_generator();
    auto name = "wilton_" + rsg.generate(8) + ".js";
#ifdef STATICLIB_WINDOWS
    auto wbuf = std::wstring();
    wbuf.resize(MAX_PATH + 1);
    auto len = ::GetTempPathW(static_cast<DWORD>(wbuf.length()), std::addressof(wbuf.front()));
    // note: unlikely failure to obtain temp dir may be ignored for one-liner purposes
    wbuf.resize(len);
    auto dir = sl::utils::narrow(wbuf);
    auto path_str = dir + "\\" + name;
#else // !STATICLIB_WINDOWS
    auto path_str = "/tmp/" + name;
#endif // STATICLIB_WINDOWS
    auto path = sl::tinydir::path(path_str);

    // prepare deps
    auto deps_line = std::string();
    auto args_line = std::string();
    if (deps.length() > 0) {
        auto parts = sl::utils::split(deps, ':');
        for (size_t i = 0; i < parts.size(); i++) {
            auto dep = parts.at(i);
            if (dep.length() > 0) {
                deps_line.append("\"").append(dep).append("\"");
                if (i < parts.size() - 1) {
                    deps_line.append(", ");
                }
                auto dep_parts = sl::utils::split(dep, '/');
                if (dep_parts.size() > 0) {
                    auto dep_name = dep_parts.back();
                    args_line.append(dep_name);
                    if (i < parts.size() - 1) {
                        args_line.append(", ");
                    }
                }
            }
        }
    }

    std::string tmpl = R"(
define([{{deps_line}}], function({{args_line}}) {
    "use strict";
    return {
        main: function() {
            var RESULT = {{code}};
            print(RESULT);
        }
    };
});)";

    // load and format template
    auto tmpl_src = sl::io::array_source(tmpl.data(), tmpl.length());
    auto src = sl::io::make_replacer_source(tmpl_src, {
        {"deps_line", deps_line},
        {"args_line", args_line},
        {"code", code}
    }, [](const std::string& err) {
        std::cerr << err << std::endl;
    });
    auto sink = path.open_write();
    sl::io::copy_all(src, sink);
    return path_str;
}

std::string choose_default_engine(const std::string& opts_script_engine_name, const std::string& debug_port) {
    if (!debug_port.empty() && !opts_script_engine_name.empty() && "duktape" != opts_script_engine_name) {
        std::cerr << "ERROR: only 'duktape' JS engine can be used for debugging" <<
                " (selected by default, if '-d' is specified)," <<
                " but another engine is requested: [" << opts_script_engine_name << "]" << std::endl;
        return std::string();
    }
    if (!debug_port.empty()) {
        return std::string("duktape");
    }
    if (!opts_script_engine_name.empty()) {
        return opts_script_engine_name;
    }
    return std::string(WILTON_DEFAULT_SCRIPT_ENGINE_STR);
}

void report_loader_error(const std::string& appdir) {
    auto conf = load_app_config(appdir);
    auto caption = std::string("wilton");
    auto msg = std::string("Application loader error");
    if (conf.has_value()) {
        auto& json = conf.value();
        caption = json["appname"].as_string(caption);
        msg = json["loadermsg"].as_string(msg);
    }
#ifdef STATICLIB_WINDOWS
    dyload_module("wilton_winscm");
    auto pars = sl::json::dumps({
        {"caption", caption},
        {"text", msg},
        {"icon", "error"},
    });
    char* out = nullptr;
    int out_len = 0;
    auto name = std::string("winscm_misc_show_message_box");
    auto err = wiltoncall(name.c_str(), static_cast<int>(name.length()),
            pars.c_str(), static_cast<int>(pars.length()),
            std::addressof(out), std::addressof(out_len));
    if (nullptr != err) {
        wilton_free(err);
    }
#else // !STATICLIB_WINDOWS
    (void) caption;
    std::cerr << msg << std::endl;
#endif // STATICLIB_WINDOWS
}

void load_pre_engine_libs(const wilton::cli::cli_options& opts, const std::string& appdir = std::string()) {
    dyload_module("wilton_logging");
    if (!opts.crypt_call_lib.empty()) {
        dyload_module(opts.crypt_call_lib);
    }
    auto name = std::string("wilton_loader");
    auto err = wilton_dyload(name.c_str(), static_cast<int>(name.length()), nullptr, 0);
    if (nullptr != err) {
        report_loader_error(appdir);
        wilton::support::throw_wilton_error(err, TRACEMSG(err));
    }
}

void load_script_engine(const std::string& script_engine,
        const std::string& wilton_home, const std::string& modurl,
        const std::vector<std::pair<std::string, std::string>>& env_vars) {
    if ("rhino" != script_engine && "nashorn" != script_engine) {
        dyload_module("wilton_" + script_engine);
    } else {
        auto exedir = wilton_home + "bin/";
        wilton::cli::jvm::load_engine(script_engine, exedir, modurl, env_vars);
    }
}

sl::support::optional<uint8_t> parse_exit_code(sl::io::span<char> span) {
    if (span.size() > 3) {
        return sl::support::optional<uint8_t>();
    }
    auto st = std::string(span.data(), span.size());
    try {
        uint16_t val16 = sl::utils::parse_uint16(st);
        if (val16 > std::numeric_limits<uint8_t>::max()) {
            return sl::support::optional<uint8_t>();
        } else {
            auto val8 = static_cast<uint8_t>(val16);
            return sl::support::optional<uint8_t>(std::move(val8));
        }
    } catch(const std::exception&) {
        return sl::support::optional<uint8_t>();
    }
}

bool check_es_module(const std::string& path) {
    auto fi = sl::tinydir::file_source(path);
    auto limited = sl::io::make_limited_source(fi, 1024);
    auto buffered = sl::io::make_buffered_source(limited);
    for (size_t i = 0; i < 32; i++) {
        auto line = buffered.read_line();
        auto trimmed = sl::utils::trim(line);
        if (sl::utils::starts_with(trimmed, "define")) {
            return false;
        }
        if (sl::utils::starts_with(trimmed, "import")) {
            return true;
        }
    }
    return false;
}

std::string create_wilton_config(const wilton::cli::cli_options& opts,
        const std::string& script_engine, const std::string& wilton_exec,
        const std::string& wilton_home, const std::string& modurl,
        std::vector<sl::json::field> paths, std::vector<sl::json::value> packages,
        std::vector<sl::json::field> env_vars, const std::string& debug_port,
        const std::string& startup_call) {
    auto config = sl::json::dumps({
        {"defaultScriptEngine", script_engine},
        {"wiltonExecutable", wilton_exec},
        {"wiltonHome", wilton_home},
        {"wiltonVersion", WILTON_VERSION_STR},
        {"requireJs", {
                {"waitSeconds", 0},
                {"enforceDefine", true},
                {"nodeIdCompat", true},
                {"baseUrl", modurl},
                {"paths", std::move(paths)},
                {"packages", std::move(packages)}
            }
        },
        {"environmentVariables", std::move(env_vars)},
// add compile-time OS
#if defined(STATICLIB_ANDROID)
        {"compileTimeOS", "android"},
#elif defined(STATICLIB_WINDOWS)
        {"compileTimeOS", "windows"},
#elif defined(STATICLIB_LINUX)
        {"compileTimeOS", "linux"},
#elif defined(STATICLIB_MAC)
        {"compileTimeOS", "macos"},
#endif // OS
        {"debugConnectionPort", debug_port},
        {"traceEnable", 0 != opts.trace_enable},
        {"cryptCall", opts.crypt_call_name}
    });
    if (0 != opts.print_config) {
        std::cout << startup_call << std::endl;
        std::cout << config << std::endl;
    }
    return config;
}

uint8_t run_new_project(const wilton::cli::cli_options& opts,
        const std::string& script_engine, const std::string& wilton_exec,
        const std::string& wilton_home, const std::string& modurl,
        std::vector<sl::json::value> packages, const std::string& debug_port,
        std::vector<sl::json::field> env_vars,
        const std::vector<std::pair<std::string, std::string>>& env_vars_pairs) {

    // startup call
    auto startup_call = sl::json::dumps({
        {"module", "wilton-newproject/index"},
        {"func", "main"},
        {"args", [&opts] {
                auto res = std::vector<sl::json::value>();
                res.emplace_back(opts.new_project);
                return res;
            }()}
    });

    // prepare wilton config
    auto config = create_wilton_config(opts, script_engine, wilton_exec, wilton_home,
            modurl, std::vector<sl::json::field>(), std::move(packages),
            std::move(env_vars), debug_port, startup_call);

    // init wilton
    auto err_init = wiltoncall_init(config.c_str(), static_cast<int> (config.length()));
    if (nullptr != err_init) {
        std::cerr << "ERROR: " << err_init << std::endl;
        wilton_free(err_init);
        return 1;
    }

    // load necessary libs
    load_pre_engine_libs(opts);

    // load script engine
    load_script_engine(script_engine, wilton_home, modurl, env_vars_pairs);

    char* out = nullptr;
    int out_len = 0;
    char* err_run = wiltoncall_runscript(script_engine.c_str(), static_cast<int>(script_engine.length()),
            startup_call.c_str(), static_cast<int> (startup_call.length()), &out, &out_len);
    auto outcleaner = sl::support::defer([out]() STATICLIB_NOEXCEPT {
        wilton_free(out);
    });
    if (nullptr != err_run) {
        std::cerr << "ERROR: " << err_run << std::endl;
        wilton_free(err_run);
        return 1;
    }
    return 0;
}

uint8_t run_startup_script(const wilton::cli::cli_options& opts,
        const std::string& script_engine, const std::string& wilton_exec,
        const std::string& wilton_home, const std::string& modurl,
        std::vector<sl::json::value> packages, const std::string& debug_port,
        std::vector<sl::json::field> env_vars,
        const std::vector<std::pair<std::string, std::string>>& env_vars_pairs,
        const std::vector<std::string>& appargs) {
    // check startup script
    auto startjs = 0 == opts.exec_one_liner ? opts.startup_script :
            write_temp_one_liner(opts.exec_deps, opts.exec_code);
    auto tmpcleaner = sl::support::defer([&opts, &startjs]() STATICLIB_NOEXCEPT {
        if (0 != opts.exec_one_liner) {
            std::remove(startjs.c_str());
        }
    });
    auto startjs_path = sl::tinydir::path(startjs);
    if (!startjs_path.exists()) {
        std::cerr << "ERROR: specified script file not found: [" + startjs + "]" << std::endl;
        return 1;
    }
    if(!startjs_path.is_regular_file()) {
        std::cerr << "ERROR: invalid script file specified: [" + startjs + "]" << std::endl;
        return 1;
    }

    // get startup module
    auto startmod = std::string();
    auto startmod_dir = std::string();
    auto startmod_id = std::string();
    auto startjs_full = sl::tinydir::full_path(startjs);
    auto appdir = sl::utils::strip_filename(startjs_full);
    std::tie(startmod, startmod_dir, startmod_id) = find_startup_module(
            opts.startup_module_name, startjs_full, appdir);
    if (startmod.empty()) {
        std::cerr << "ERROR: cannot determine startup module name, use '-s' to specify it" << std::endl;
        return 1;
    }

    // prepare paths
    auto paths = prepare_paths(wilton_home, opts.binary_modules_paths, startmod, startmod_dir);

    // prepare args
    auto args_json = std::vector<sl::json::value>();
    for (auto& st : appargs) {
        args_json.emplace_back(st);
    }

    // startup call
    auto startup_call = std::string();
    if (0 != opts.load_only) {
        startup_call = sl::json::dumps({
            {"module", startmod_id}
        });
    } else if (0 != opts.es_module || check_es_module(startjs_full)) {
        startup_call = sl::json::dumps({
            {"esmodule", "file://" + startjs_full},
            {"args", std::move(args_json)}
        });
    } else {
        startup_call = sl::json::dumps({
            {"module", startmod_id},
            {"func", "main"}, // optional, kept for compat
            {"args", std::move(args_json)}
        });
    }
    // prepare wilton config
    auto config = create_wilton_config(opts, script_engine, wilton_exec, wilton_home,
            modurl, std::move(paths), std::move(packages), std::move(env_vars),
            debug_port, startup_call);

    // init wilton
    auto err_init = wiltoncall_init(config.c_str(), static_cast<int> (config.length()));
    if (nullptr != err_init) {
        std::cerr << "ERROR: " << err_init << std::endl;
        wilton_free(err_init);
        return 1;
    }

    // load necessary libs
    load_pre_engine_libs(opts, appdir);

    // load script engine
    load_script_engine(script_engine, wilton_home, modurl, env_vars_pairs);

    // init signals/ctrl+c to allow their use from js
    if ("rhino" != script_engine && "nashorn" != script_engine) {
        init_signals();
    }

    // call script
    char* out = nullptr;
    int out_len = 0;
    char* err_run = wiltoncall_runscript(script_engine.c_str(), static_cast<int>(script_engine.length()),
            startup_call.c_str(), static_cast<int> (startup_call.length()), &out, &out_len);
    auto outcleaner = sl::support::defer([out]() STATICLIB_NOEXCEPT {
        wilton_free(out);
    });
    if (nullptr != err_run) {
        std::cerr << "ERROR: " << err_run << std::endl;
        wilton_free(err_run);
        return 1;
    } else if (out_len > 0) {
        auto opt = parse_exit_code({out, out_len});
        if (opt.has_value()) {
            return opt.value();
        } // pass through
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        // parse launcher args
        int launcher_argc = find_launcher_args_end(argc, argv);
        wilton::cli::cli_options opts(launcher_argc, argv);
        
        // collect app args
        auto appargs = std::vector<std::string>();
        for (int i = launcher_argc + 1; i < argc; i++) {
            appargs.emplace_back(argv[i]);
        }

        // check invalid options
        if (!opts.parse_error.empty()) {
            std::cerr << "ERROR: " << opts.parse_error << std::endl;
            std::cerr << opts.usage() << std::endl;
            return 1;
        }

        // show help
        if (0 != opts.help) {
            std::cout << opts.usage() << std::endl;
            poptPrintHelp(opts.ctx, stdout, 0);
            return 0;
        }

        // show version
        if (0 != opts.version) {
            std::cout << std::string(WILTON_VERSION_STR) << std::endl;
            return 0;
        }

        // get wilton home
        auto wilton_exec = sl::tinydir::normalize_path(sl::utils::current_executable_path());
        auto wilton_home = sl::utils::strip_filename(sl::tinydir::normalize_path(sl::utils::strip_filename(wilton_exec)));

        // set environment vars
        set_env_vars(opts.environment_vars);

        // check whether GHC mode is requested
        if (0 != opts.ghc_init) {
            wilton::cli::ghc::init_and_run_main(wilton_home, opts.startup_script, appargs);
            return 0;
        }

        // check modules dir
        auto moddir = !opts.modules_dir_or_zip.empty() ? opts.modules_dir_or_zip : wilton_home + "std.wlib";
        auto modpath = sl::tinydir::path(moddir);
        if (!modpath.exists()) {
            std::cerr << "ERROR: specified modules directory (or wlib bundle) not found: [" + moddir + "]" << std::endl;
            return 1;
        }
        auto modurl = modpath.is_directory() ?
                std::string("file://") + sl::tinydir::full_path(moddir) :
                std::string("zip://") + moddir;
        if (modpath.is_directory() && '/' != modurl.at(modurl.length() - 1)) {
            modurl.push_back('/');
        }

        // packages
        auto packages = load_packages_list(modurl);

        // get debug connection port, may be switched to int and defaulted to -1 eventually
        auto debug_port = !opts.debug_port.empty() ? opts.debug_port : std::string("");

        // get script engine name
        auto script_engine = choose_default_engine(opts.script_engine_name, debug_port);
        if (script_engine.empty()) {
            return 1;
        }

        // env vars
        auto env_vars = collect_env_vars();
        auto env_vars_pairs = std::vector<std::pair<std::string, std::string>>();
        for (auto& fi : env_vars) {
            env_vars_pairs.emplace_back(fi.name(), fi.as_string_or_throw(fi.name()));
        }

        // check whether new-project requested
        uint8_t rescode = 0;
        if (!opts.new_project.empty()) {
            rescode = run_new_project(opts, script_engine, wilton_exec, wilton_home,
                    modurl, std::move(packages), debug_port, std::move(env_vars),
                    env_vars_pairs);
        } else {
            rescode = run_startup_script(opts, script_engine, wilton_exec, wilton_home,
                    modurl, std::move(packages), debug_port, std::move(env_vars),
                    env_vars_pairs, appargs);
        }
        return rescode;

    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }
}
