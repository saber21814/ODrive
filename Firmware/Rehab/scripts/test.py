#!/usr/bin/env python3
"""Compile and run the existing Rehab host tests; no board or ARM tools are used.

Run from any directory:
    python Firmware/Rehab/scripts/test.py
    python Firmware/Rehab/scripts/test.py --quick

The default includes the 24-hour virtual service test and upstream native tests.
Quick mode skips the long virtual run and native tests. Compiler/run logs and
report.json are written under Firmware/build/rehab/tests. Simulation CSV is kept
separately so its stdout remains a usable data artifact.
"""

import argparse
import datetime
import itertools
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time


ROOT = pathlib.Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "build" / "rehab" / "tests"
LOGS = OUTPUT / "logs"
BINARIES = OUTPUT / "bin"
REPORT = OUTPUT / "report.json"

CORE = ["Rehab/core/engine.cpp"]
FRAMING = ["Rehab/protocol/frame.cpp"]
SERVICE = CORE + FRAMING + [
    "Rehab/protocol/config_fields.cpp", "Rehab/protocol/service.cpp"
]
STRICT = ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-pedantic"]
GNU_STRICT = ["-std=gnu++17", "-O2", "-Wall", "-Wextra", "-Werror"]


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def relative(path):
    try:
        return pathlib.Path(path).relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


def suite(name, sources, *, flags=None, includes=None, definitions=None,
          arguments=None, production=None, notes=None, timeout=120):
    """Describe only existing test/production sources; do not generate code."""
    return {
        "name": name,
        "sources": sources,
        "flags": list(STRICT if flags is None else flags),
        "includes": list(["."] if includes is None else includes),
        "definitions": list(definitions or []),
        "arguments": list(arguments or []),
        "production_sources": list(production or []),
        "notes": list(notes or []),
        "run_timeout_s": timeout,
    }


def specifications(quick):
    suites = [
        suite("core", CORE + ["Tests/rehab/core_tests.cpp"], production=CORE),
        suite("protocol", FRAMING + ["Tests/rehab/protocol_tests.cpp"], production=FRAMING),
        suite("service", SERVICE + ["Tests/rehab/service_tests.cpp"],
              production=SERVICE, arguments=[] if quick else ["--long"],
              notes=["24-hour virtual run skipped by --quick." if quick else
                     "Includes 24-hour virtual time, 20 microsecond-clock wraps, and energy-reference checks."]),
    ]
    for application, training, statistics, compensation in itertools.product((0, 1), repeat=4):
        suites.append(suite(
            "service_a{}t{}s{}c{}".format(application, training, statistics, compensation),
            SERVICE + ["Tests/rehab/service_tests.cpp"], production=SERVICE,
            definitions=[
                "REHAB_ENABLE_APPLICATION={}".format(application),
                "REHAB_ENABLE_TRAINING={}".format(training),
                "REHAB_ENABLE_STATISTICS={}".format(statistics),
                "REHAB_ENABLE_COMPENSATION={}".format(compensation),
            ],
            notes=["Business service feature-combination regression; long virtual run is exercised by the primary service suite."],
        ))
    for training, statistics, compensation in itertools.product((0, 1), repeat=3):
        definitions = [
            "REHAB_ENABLE_TRAINING={}".format(training),
            "REHAB_ENABLE_STATISTICS={}".format(statistics),
            "REHAB_ENABLE_COMPENSATION={}".format(compensation),
        ]
        suites.append(suite(
            "features_t{}s{}c{}".format(training, statistics, compensation),
            CORE + ["Tests/rehab/feature_switch_tests.cpp"],
            definitions=definitions, production=CORE,
            flags=STRICT + ["-Wdouble-promotion"],
        ))

    suites.append(suite('bench_session',['Tests/rehab/bench_session_tests.cpp'],
        production=['Rehab/platform/bench_session.hpp']))
    for application in (0,1):
        suites.append(suite('bench_service_a'+str(application), SERVICE+['Tests/rehab/bench_service_tests.cpp'],
            definitions=['REHAB_ENABLE_APPLICATION='+str(application)],production=SERVICE))
    for name,source,production in (
        ('bench_client','Tests/rehab/bench_client_tests.py','Rehab/scripts/bench.py'),
        ('bench_build','Tests/rehab/bench_build_tests.py','Rehab/scripts/build.py')):
        item=suite(name,[source],production=[production]);item['python']=True;suites.append(item)

    driver_sources = [
        "Rehab/drivers/spi_arbiter.cpp", "Rehab/drivers/hx711.cpp", "Rehab/drivers/ch394.cpp"
    ]
    suites.append(suite(
        "driver_models", ["Tests/rehab/driver_model_tests.cpp"] + driver_sources,
        flags=GNU_STRICT, definitions=["REHAB_V12"],
        includes=["Tests/rehab/driver_mocks", "Drivers/STM32", "."],
        production=driver_sources,
        notes=["Actual V1.2 SPI arbiter, HX711 and CH394 sources; HAL/register models do not verify electrical timing."],
    ))
    driver_v13 = next(s for s in suites if s['name'] == 'driver_models').copy()
    driver_v13['name'] = 'driver_models_v13'
    driver_v13['definitions'] = driver_v13['definitions'] + ['REHAB_V13=1']
    driver_v13['notes'] = ['V1.3 driver timeout snapshots; modeled SPI faults and registers, not electrical validation.']
    suites.append(driver_v13)
    driver_v13_real = driver_v13.copy()
    driver_v13_real["name"] = "driver_models_v13_real_flag"
    driver_v13_real["definitions"] = driver_v13["definitions"] + ["REHAB_REAL_MOTOR=1"]
    suites.append(driver_v13_real)

    suites.append(suite(
        "isr_timing", ["Tests/rehab/isr_timing_tests.cpp", "Rehab/platform/isr_timing.cpp"],
        flags=GNU_STRICT, includes=["Tests/rehab/timing_mocks", "."],
        production=["Rehab/platform/isr_timing.cpp"],
        notes=["Production DWT accounting with modeled cycle counter; not measured hardware latency or WCET."],
    ))
    runtime_definitions = [
        "REHAB_V12", "REHAB_ENABLE_APPLICATION=1", "REHAB_ENABLE_PROTOCOL=1",
        "REHAB_ENABLE_HX711=0", "REHAB_ENABLE_NETWORK=0",
        "REHAB_ENABLE_STATISTICS=1", "REHAB_ENABLE_TRAINING=1",
        "REHAB_ENABLE_COMPENSATION=1", "REHAB_STAGE=6",
    ]
    for board, real_motor in itertools.product(('v12','v13'), (0,1)):
        suites.append(suite(
            ("runtime" if not real_motor else "runtime_real_flag")+('_v13' if board=='v13' else ''),
            ["Tests/rehab/runtime_tests.cpp", "Rehab/platform/isr_timing.cpp"] + SERVICE,
            flags=GNU_STRICT,
            definitions=runtime_definitions + ["REHAB_REAL_MOTOR={}".format(real_motor)] + (["REHAB_V13=1"] if board=='v13' else []),
            includes=["Tests/rehab/runtime_mocks", "Tests/rehab/driver_mocks",
                      "Drivers/STM32", "fibre-cpp/include", "."],
            production=SERVICE + ["Rehab/platform/runtime.cpp", "fibre-cpp/protocol.hpp"],
            notes=[
                "runtime_tests.cpp directly includes the actual runtime.cpp; do not compile runtime.cpp a second time.",
                "HAL/RTOS/ODrive carrier objects are host models. The real-motor flag remains inhibited by unverified commissioning declarations.",
            ],
        ))

    network_runtime = next(s for s in suites if s['name'] == 'runtime_v13').copy()
    network_runtime['name'] = 'runtime_network_v13'
    network_runtime['definitions'] = [d.replace('REHAB_ENABLE_NETWORK=0', 'REHAB_ENABLE_NETWORK=1')
                                      for d in network_runtime['definitions']]
    network_runtime['notes'] = network_runtime['notes'] + [
        'Actual SetNetwork admission and wire response; CH394 transport is a disconnected host carrier.']
    suites.append(network_runtime)
    bench_runtime=network_runtime.copy()
    bench_runtime['name']='bench_runtime_v13'
    bench_runtime['definitions']=network_runtime['definitions']+['REHAB_BENCH=1']
    bench_runtime['sources']=['Tests/rehab/bench_runtime_tests.cpp','Rehab/platform/isr_timing.cpp']+SERVICE
    bench_runtime['production_sources']=network_runtime['production_sources']+['Rehab/platform/bench_native.hpp','Rehab/platform/bench_session.hpp']
    bench_runtime['notes']=['Actual bench adapter/runtime; native algorithms and hardware are mocked, not physical acceptance.']
    suites.append(bench_runtime)


    suites.append(suite(
        "native_ingress", ["Tests/rehab/ingress_tests.cpp", "Rehab/platform/isr_timing.cpp"] + SERVICE,
        flags=GNU_STRICT + ["-Wno-unused-parameter"], definitions=runtime_definitions,
        includes=["Tests/rehab/ingress/mocks", "Tests/rehab/driver_mocks", "Drivers/STM32",
                  "fibre-cpp/include", "build/rehab/ingress_generated", "."],
        production=SERVICE + ["Rehab/platform/runtime.cpp", "communication/ascii_protocol.cpp",
                    "communication/can/can_simple.cpp", "MotorControl/axis.cpp",
                    "MotorControl/pwm_input.cpp", "MotorControl/low_level.cpp", "fibre-cpp/protocol.hpp"],
        notes=[
            "Fresh source preparation extracts 52 complete production functions unchanged; manifest records source/function hashes and caller checks.",
            "Real generated Fibre interfaces/stubs and complete runtime; HAL/RTOS, native object carriers and the lookup directory are mocked.",
            "Bounded actual step_cb boundary/seeded regression supplements the excluded upstream 2^33-iteration test.",
        ],
    ))

    suites.append(suite(
        "stream", ["Tests/rehab/stream_tests.cpp", "Rehab/platform/stream.cpp"],
        flags=GNU_STRICT, includes=[".", "fibre-cpp/include"],
        production=["Rehab/platform/stream.cpp", "fibre-cpp/include/fibre/async_stream.hpp"],
        notes=["Actual Stream adapter and Fibre async interfaces; endpoint and byte-queue carriers test partial transfers, reconnects and delayed completion callbacks."],
    ))
    for board in ('native','v12','v13','bench'):
        suites.append(suite(
            {'native':'nvm_native','v12':'nvm_rehab','v13':'nvm_rehab_v13','bench':'nvm_bench_v13'}[board], ["Tests/rehab/nvm_tests.cpp"],
            includes=[".", "fibre-cpp/include"],
            definitions=(["REHAB_V12=1"] if board!='native' else [])+(["REHAB_V13=1"] if board in ('v13','bench') else [])+(['REHAB_BENCH=1'] if board=='bench' else []),
            production=["MotorControl/nvm_config.hpp", "Drivers/STM32/stm32_nvm.h", "fibre-cpp/crc.hpp"],
            notes=["Actual ConfigManager and CRC implementation with an in-memory NVM carrier; tests two-pass persistence, version-specific CRC seeds, truncation, corruption and interrupted writes."],
        ))

    suites.append(suite('phase_adc_monitor',['Tests/rehab/phase_adc_monitor_tests.cpp'],
        production=['Rehab/platform/phase_adc_monitor.hpp'],
        notes=['DC ADC observer packing and stale/invalid handling; no physical current qualification.']))
    suites.append(suite('temperature_v13',['Tests/rehab/temperature_tests.cpp'],
        definitions=['REHAB_V13=1'],production=['Rehab/platform/temperature.hpp','Rehab/platform/commissioning.hpp']))
    suites.append(suite('v13_gate',['Tests/rehab/v13_gate_tests.cpp'],
        includes=['Tests/rehab/v13_gate_mocks','.'],
        production=['Rehab/drivers/v13_gate_driver.hpp'],
        notes=['Actual V1.3 wrapper with a native-driver carrier; real DRV SPI writes/readbacks also covered by ARM compilation, physical behavior remains unverified.']))
    contract=suite('v13_board_contract',['Tests/rehab/v13_board_contract_test.py'],
        notes=['Static production-source contract: pin/ADC/DMA ownership and cold-boot ordering; not measured MCU behavior.'])
    contract['python']=True
    suites.append(contract)
    protection=suite('core_protection_v13',['Tests/rehab/core_protection_v13_tests.py'],
        production=['Rehab/scripts/core_protection_v13.py'],
        notes=['Actual guard with injected Git/hash results; rejects staged core additions and protected-file changes without changing the filesystem/index.'])
    protection['python']=True
    suites.append(protection)
    entry=suite('project_entry',['Tests/rehab/project_entry_tests.py'],
        production=['Rehab/scripts/project.py'],
        notes=['Hardware-free build/flash preflight regression; programmer subprocess is mocked, never executed.'])
    entry['python']=True
    suites.append(entry)
    native_sources = sorted(relative(path) for path in (ROOT / "Tests").glob("*.cpp"))
    native = suite(
        "native", native_sources,
        flags=["-std=c++17", "-O3"],
        includes=[".", "MotorControl", "fibre-cpp/include", "Drivers/DRV8301", "doctest"],
        arguments=["--test-case-exclude=step_cb"],
        production=["MotorControl/timer.hpp", "MotorControl/utils.hpp", "communication/can/can_helpers.hpp"],
        notes=[
            "Uses the existing upstream Tests/*.cpp and original Tupfile host compiler flags.",
            "step_cb is excluded to keep its exceptionally long loop out of bounded automated verification; it is not reported as passing.",
            "The existing upstream trajectory test contains its original test-local implementation; this suite is regression evidence, not validation of real FOC or the real trajectory implementation.",
        ],
    )
    if quick:
        native["skip_reason"] = "Native tests skipped by --quick."
    suites.append(native)
    simulation = suite(
        "simulation", CORE + ["Tests/rehab/sim_main.cpp"],
        arguments=["60"], production=CORE,
        notes=["60-second deterministic mass/load/actuator model and controlled stop; software only."],
    )
    simulation["stdout_artifact"] = LOGS / "simulation.csv"
    suites.append(simulation)
    return suites


def log_tail(path, limit=6000):
    if not pathlib.Path(path).exists():
        return ""
    with pathlib.Path(path).open("rb") as stream:
        stream.seek(0, os.SEEK_END)
        size = stream.tell()
        stream.seek(max(0, size - limit))
        return stream.read().decode("utf-8", errors="replace")


def execute(command, log_path, environment, timeout, stdout_artifact=None):
    started = time.monotonic()
    details = {
        "command": [str(part) for part in command],
        "cwd": str(ROOT),
        "log": relative(log_path),
        "timeout_s": timeout,
        "exit_code": None,
        "status": "failed",
    }
    if stdout_artifact:
        details["stdout_artifact"] = relative(stdout_artifact)
    try:
        with log_path.open("w", encoding="utf-8", newline="\n") as log:
            log.write("COMMAND (argument array): " + json.dumps(details["command"], ensure_ascii=False) + "\n")
            log.write("WORKING DIRECTORY: " + str(ROOT) + "\n\n")
            log.flush()
            if stdout_artifact:
                with stdout_artifact.open("wb") as stdout:
                    completed = subprocess.run(
                        details["command"], cwd=ROOT, env=environment,
                        stdout=stdout, stderr=log, timeout=timeout, check=False,
                    )
            else:
                completed = subprocess.run(
                    details["command"], cwd=ROOT, env=environment,
                    stdout=log, stderr=subprocess.STDOUT, timeout=timeout, check=False,
                )
            details["exit_code"] = completed.returncode
            details["status"] = "passed" if completed.returncode == 0 else "failed"
            log.write("\nEXIT CODE: {}\n".format(completed.returncode))
    except subprocess.TimeoutExpired:
        details["error"] = "Process exceeded {} seconds and was terminated.".format(timeout)
    except OSError as error:
        details["error"] = str(error)
    details["elapsed_s"] = round(time.monotonic() - started, 3)
    if details.get("error"):
        with log_path.open("a", encoding="utf-8") as log:
            log.write("\nERROR: " + details["error"] + "\n")
    return details


def run_suite(specification, compiler, environment):
    name = specification["name"]
    result = {key: value for key, value in specification.items() if key != "stdout_artifact"}
    result["status"] = "failed"
    if "skip_reason" in specification:
        result["status"] = "skipped"
        print("SKIP {}: {}".format(name, specification["skip_reason"]), flush=True)
        return result
    if specification.get('python'):
        result['run']=execute([sys.executable,'-B',*specification['sources']],
            LOGS/(name+'.run.log'),environment,specification['run_timeout_s'])
        result['status']=result['run']['status']
        print(('PASS' if result['status']=='passed' else 'FAIL')+' '+name,flush=True)
        if result['status']!='passed': print(log_tail(LOGS/(name+'.run.log')),flush=True)
        return result
    if name=="native_ingress":
        result["prepare"]=execute(
            [sys.executable,"-B","Rehab/scripts/ingress_sources.py","--output","build/rehab/ingress_generated"],
            LOGS/(name+".prepare.log"),environment,60)
        result["provenance_manifest"]="build/rehab/ingress_generated/manifest.json"
        if result["prepare"]["status"]!="passed":
            result["error"]="Production ingress source preparation failed."
            print("FAIL {} (prepare)\n{}".format(name,log_tail(LOGS/(name+".prepare.log"))),flush=True)
            return result
    missing = [path for path in specification["sources"] + specification["production_sources"]
               if not (ROOT / path).is_file()]
    if missing or not specification["sources"]:
        result["error"] = "Missing test/production sources: {}".format(", ".join(missing) or "empty source list")
        print("FAIL {}: {}".format(name, result["error"]), flush=True)
        return result
    binary = BINARIES / (name + (".exe" if os.name == "nt" else ""))
    command = ([compiler] + specification["flags"] +
               ["-D" + definition for definition in specification["definitions"]] +
               ["-I" + include for include in specification["includes"]] +
               specification["sources"] + ["-o", str(binary)])
    print("BUILD {}".format(name), flush=True)
    result["compile"] = execute(command, LOGS / (name + ".build.log"), environment, 180)
    if result["compile"]["status"] != "passed":
        result["error"] = "Compilation failed; the binary was not executed."
        print("FAIL {} (compile)\n{}".format(name, log_tail(LOGS / (name + ".build.log"))), flush=True)
        return result
    result["binary"] = relative(binary)
    result["run"] = execute(
        [str(binary)] + specification["arguments"], LOGS / (name + ".run.log"),
        environment, specification["run_timeout_s"], specification.get("stdout_artifact"),
    )
    result["status"] = result["run"]["status"]
    label = "PASS" if result["status"] == "passed" else "FAIL"
    print("{} {} ({:.3f} s run; {})".format(
        label, name, result["run"]["elapsed_s"], result["run"]["log"]), flush=True)
    if result["status"] != "passed":
        print(log_tail(LOGS / (name + ".run.log")), flush=True)
    return result


def save_report(report, started):
    counts = {status: sum(test["status"] == status for test in report["tests"])
              for status in ("passed", "failed", "skipped")}
    report["summary"] = dict(counts, total=len(report["tests"]))
    report["elapsed_s"] = round(time.monotonic() - started, 3)
    report["updated_at_utc"] = utc_now()
    temporary = REPORT.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary.replace(REPORT)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--quick", action="store_true", help="Skip the 24-hour virtual run and upstream native tests.")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"), help="Host g++ executable (default: CXX or g++ from PATH).")
    args = parser.parse_args()
    for directory in (OUTPUT, LOGS, BINARIES):
        directory.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    report = {
        "schema_version": 1,
        "started_at_utc": utc_now(),
        "firmware_root": str(ROOT),
        "mode": "quick" if args.quick else "full",
        "status": "running",
        "hardware_access": False,
        "compiler": {"requested": args.cxx},
        "tests": [],
        "coverage_limits": [
            "Host tests use production application/driver/runtime sources and explicit hardware carriers.",
            "No ARM build, flashing, motor drive, physical waveform, interrupt timing or thermal validation is performed.",
        ],
    }
    try:
        compiler = shutil.which(args.cxx)
        if not compiler:
            raise RuntimeError("Host g++ was not found: {!r}. Add MinGW GCC to PATH or pass --cxx.".format(args.cxx))
        compiler = str(pathlib.Path(compiler).resolve())
        environment = os.environ.copy()
        # Also expose compiler runtime DLLs when --cxx is an absolute MinGW path.
        environment["PATH"] = str(pathlib.Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
        report["compiler"]["path"] = compiler
        probe = execute([compiler, "--version"], LOGS / "compiler.log", environment, 30)
        report["compiler"]["probe"] = probe
        if probe["status"] != "passed":
            raise RuntimeError("Host compiler did not run successfully; see {}.".format(probe["log"]))
        report["compiler"]["version_output"] = log_tail(LOGS / "compiler.log")
        save_report(report, started)
        for specification in specifications(args.quick):
            report["tests"].append(run_suite(specification, compiler, environment))
            save_report(report, started)
        report["status"] = "failed" if any(test["status"] == "failed" for test in report["tests"]) else "passed"
    except KeyboardInterrupt:
        report["status"] = "interrupted"
        report["error"] = "Interrupted by user; completed test results are retained."
    except (OSError, RuntimeError) as error:
        report["status"] = "failed"
        report["error"] = str(error)
        print("FAIL: " + str(error), file=sys.stderr, flush=True)
    finally:
        save_report(report, started)
    counts = report["summary"]
    print("{}: {} passed, {} failed, {} skipped; report {}".format(
        report["status"].upper(), counts["passed"], counts["failed"], counts["skipped"], REPORT), flush=True)
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
