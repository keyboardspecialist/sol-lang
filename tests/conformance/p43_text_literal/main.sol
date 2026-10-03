module conformance.p43_text_literal

@entry
public function launch() -> Int64 effects { pure } {
    let empty = ""
    let hello = "hello"
    let copied = hello
    let same = "same"
    let equal = copied == hello && same == "same"
    let different = "ab" != "ac"
    return if equal && different { 1 } else { 0 }
}
