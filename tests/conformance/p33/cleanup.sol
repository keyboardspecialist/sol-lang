module cleanup

enum PolicyFailure { unavailable }

/* The normal host-result path reaches the failure-result ensure.  A runtime
 * host failure instead follows cleanup to RESUME_FAILURE, which makes this a
 * focused P3.3 suppression-authentication fixture. */
capability FallibleSource {
    function read() -> Int64 effects { fixture.read<Self> }
}

function host_policy(source: capability FallibleSource) -> Result<Int64, PolicyFailure>
effects { fixture.read<source> }
ensures { failure => true }
{
    let value = source.read()
    return ok(value)
}

function nested(value: Int64) -> Int64 effects { pure } {
    let outer = "outer"
    region guarded {
        {
            let inner = "inner"
            let checked = value + 1
        }
    }
    value + 2
}

function branching(value: Int64, choose: Bool) -> Int64 effects { pure } {
    let outer = "outer"
    if choose {
        {
            let inner = "inner"
            inner == "inner"
            value + 1
        }
    } else {
        while false decreases { value } {
            let loop_value = "loop"
        }
        value + 2
    }
}

test "nested implicit cleanup" nested(1) == 2
test "branching implicit cleanup" branching(1, true) == 2
