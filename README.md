# fdstream

`std::istream` implementation that uses Linux `splice(2)` to
accelerate skipping data for pipe inputs
