module conformance.p44f_site_domains_exclusive

function increment(value: inout Int64) -> () effects { pure } {
    value += 1
}

function checked() -> Int64 effects { pure } ensures { result == 43 } {
    var value = 41
    let callback = increment
    callback(value)
    return value
}

@entry
public function launch() -> Int64 effects { pure } { return checked() + 1000 }
public function other() -> Int64 effects { pure } { return checked() + 1001 }
