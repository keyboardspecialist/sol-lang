module conformance.p43_method_meter.main

use conformance.p43_method_meter.definitions.Counter

function first(value: Int64) -> Int64 effects { pure } { return value.read() }
function second(value: Int64) -> Int64 effects { pure } { return value.read() }

@entry
public function launch() -> Int64 effects { pure } {
    let one = 1
    let two = 2
    return first(40) + second(41) + one.read() + two.read()
}
