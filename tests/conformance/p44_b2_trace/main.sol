module conformance.p44_b2_trace

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
public function launch() -> Int64 effects { pure } {
    return fold("trace")
}
