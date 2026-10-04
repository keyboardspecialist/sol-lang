module conformance.p44_b2_trace_stress

function fold(parameter: Text) -> Int64 effects { pure } {
    let check = parameter == "trace"
    let result = match "temporary" {
        _ => 43
    }
    region owned_parameter {
        {
            let inner = "inner"
        }
    }
    return result
}

@entry
public function launch() -> Int64 effects { panic } {
    let packet = if false {
        panic "stress packet"
    } else {
        0
    }
    let first = fold("trace")
    let second = fold("trace")
    let third = fold("trace")
    let fourth = fold("trace")
    let fifth = fold("trace")
    return fifth
}
