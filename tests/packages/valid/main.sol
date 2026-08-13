module example.main

use example.models.Counter
use example.models.create
use example.models.render
use example.rules.nonnegative
use example.services.Reader
use example.services.read_value

function run(reader: capability Reader) -> Int64 effects { service.read<reader> }
requires { nonnegative(7) }
ensures { result >= 0 }
{
    let counter = create(7)
    let label = render(counter)
    return read_value(reader, counter.value)
}
