module conformance.p44c_refined_constant

type Always = refined Int64 where true

function always(value: Int64) -> Always effects { pure } {
    return Always(value)
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = always(-1)
    if value == value { return 7 } else { return 0 }
}
