module conformance.p43_propagate_result_residual

function adapt(value: Result<Int64, Text>) -> Result<Bool, Text> effects { pure } {
    let item = value?
    return ok(item == 7)
}

@entry
public function launch() -> Int64 effects { pure } {
    return if adapt(err("error")) == err("error") { 104 } else { 0 }
}
