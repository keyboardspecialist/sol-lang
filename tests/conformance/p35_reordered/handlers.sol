module p35.handlers

/* Source relocation fixture: no handler semantic key may depend on source
 * paths, comments, spans, or dense construction order. */

capability Console {
    function write(value: Text) -> () effects { console.write<Self> }
}

capability Provider {
    /* P3.5 providers are bodyful pure capability members: this lowers their
       binding to TARGET_INTERNAL, never to a host/import target. */
    function write(value: Text) -> () effects { pure } { return () }
}


/* The nested same-root case is intentionally the source accepted by the P2
 * handler tests.  P3.5 consumes it as a nonempty exact-frame fixture. */
function newest(console: capability Console, first: capability Provider,
    second: capability Provider) -> () {
    return handle console.write<console> with first {
        handle console.write<console> with second { console.write("one") }
    }
}

/* Equal capability types do not imply equal opaque roots.  The inner frame
 * cannot observe the left-root dispatch, which therefore falls through. */
function distinct_roots(left: capability Console, right: capability Console,
    first: capability Provider, second: capability Provider) -> () {
    return handle console.write<left> with first {
        handle console.write<right> with second { left.write("two") }
    }
}

function one_handler(console: capability Console, provider: capability Provider)
    -> () {
    return handle console.write<console> with provider { console.write("three") }
}

function cleanup_return(console: capability Console, provider: capability Provider)
    -> () {
    handle console.write<console> with provider { return console.write("four") }
    return ()
}

function cleanup_failure(console: capability Console, provider: capability Provider)
    -> () effects { panic } {
    return handle console.write<console> with provider {
        let zero = 1 / 0
        return console.write("five")
    }
}
