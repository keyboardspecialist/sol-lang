module conformance.p44f_site_domains_execution
function checked(v: Int64, divisor: Int64) -> Int64 effects { pure }
ensures { result / divisor > 0 } {
    if v > 0 { return v } else { return -1 }
}
@entry
public function false_true() -> Int64 effects { pure } { return checked(4, -1) }
