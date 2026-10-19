module conformance.p44f1b_predicate_nonleaf

function scalar_leaf(v: Int64) -> Bool effects { pure } {
    return v > 0
}

function helper(v: Int64) -> Bool effects { pure } {
    return scalar_leaf(v)
}

function core(v: Int64) -> Int64 effects { pure }
requires { helper(v) }
{
    return v
}

@entry
public function launch() -> Int64 effects { pure } {
    return core(4)
}

function text_leaf(v: Int64) -> Bool effects { pure } {
    if v < 0 {
        let description = "unexecuted text path"
        return description == "reject rich target"
    } else {
        return v > 0
    }
}

function rich_core(v: Int64) -> Int64 effects { pure }
requires { text_leaf(v) }
{
    return v
}

public function rich_caller() -> Int64 effects { pure } {
    return rich_core(4)
}
