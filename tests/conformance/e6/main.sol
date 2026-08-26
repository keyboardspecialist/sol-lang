module conformance.main

use conformance.core.compute
use conformance.core.fail_runtime
use conformance.core.increment_option
use conformance.core.increment_result
use conformance.domain.Failure

capability Console {
    function write(value: Text) -> ()
    effects { console.write<Self> }
}

capability Arguments {
    function count() -> Int64
    effects { process.arguments.count<Self> }

    function get(index: Int64) -> Option<Text>
    effects { process.arguments.get<Self> }
}

capability Configuration {
    function read(key: Text) -> Option<Text>
    effects { configuration.read<Self> }
}

test "contracted ownership core" compute(4, false) == ok(9)

test "typed error path" compute(4, true) == err(Failure.invalid)

test "option propagation" {
    increment_option(some(4)) == some(5)
    && increment_option(none()) == none()
}

test "result propagation" {
    increment_result(ok(4)) == ok(5)
    && increment_result(err(Failure.invalid)) == err(Failure.invalid)
}

@entry
public function launch(
    console: capability Console,
    arguments: capability Arguments,
    configuration: capability Configuration,
) -> Int64
effects {
    configuration.read<configuration>
    console.write<console>
    panic
    process.arguments.count<arguments>
    process.arguments.get<arguments>
}
{
    let mode = configuration.read("mode")
    if mode == some("panic") {
        fail_runtime()
        return 1
    } else {
        let first = arguments.get(0)
        let count = arguments.count()
        let result = compute(4, false)
        if mode == some("e6")
        && first == some("fixture")
        && count == 1
        && result == ok(9)
        {
            console.write("E6 ok\n")
            return 0
        } else {
            console.write("E6 failed\n")
            return 1
        }
    }
}
