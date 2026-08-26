module conformance.core

use conformance.domain.Envelope
use conformance.domain.Failure
use conformance.domain.Packet
use conformance.domain.Pair
use conformance.domain.Positive
use conformance.domain.identity
use conformance.domain.score

function increment(value: inout Int64) -> ()effects { pure } {
    value += 1
}

public function increment_option(value: Option<Int64>) -> Option<Int64>
effects { pure }
{
    let item = value?
    return some(item + 1)
}

public function increment_result(
    value: Result<Int64, Failure>,
) -> Result<Int64, Failure>
effects { pure }
{
    let item = value?
    return ok(item + 1)
}

public function extract(value: Envelope<Int64>) -> Int64 effects { pure } {
    return match value {
        wrapped(Packet { data = (number, true) }) if number > 0 => number
        wrapped(Packet { ignored = ignored, data = (number, true) }) => number + 10
        _ => 0
    }
}

@stable("conformance.compute.v1")
public function compute(value: Int64, fail: Bool) -> Result<Int64, Failure>
effects { pure }
requires { value > 0 }
ensures {
    success => result >= old(value)
    failure => true
}
{
    let checked = Positive(value)
    var pair = Pair { left = identity(value), right = score(value) }
    increment(pair.left)
    region temporary {
        let disposable = Pair { left = pair.left, right = pair.right }
    }
    if fail {
        return err(Failure.invalid)
    } else {
        let packet = Packet<Int64> {
            ignored = false,
            data = (pair.left, true),
        }
        let envelope = Envelope<Int64>.wrapped(packet = packet)
        return ok(extract(envelope) + pair.right)
    }
}

public function fail_runtime() -> ()effects { panic } {
    let pending = Pair { left = 1, right = 2 }
    panic "conformance failure"
}
