| Pattern | Round | Build | msg/s | Submit CPU µs/msg | Submit heap calls/msg | Source heap/msg | Target heap/msg |
|---|---:|---|---:|---:|---:|---:|---:|
| request-backpressure | 1 | W | 43,986.4 | 21.785 | 41.386 | 59.221 | 43.396 |
| request-backpressure | 1 | main | 42,698.5 | 22.541 | 49.385 | 67.997 | 51.810 |
| request-backpressure | 2 | W | 42,870.1 | 22.539 | 41.347 | 59.693 | 44.610 |
| request-backpressure | 2 | main | 42,381.8 | 22.448 | 49.468 | 67.949 | 51.732 |
| request-backpressure | 3 | W | 44,970.1 | 21.493 | 41.307 | 59.065 | 43.558 |
| request-backpressure | 3 | main | 43,619.6 | 22.099 | 49.297 | 67.289 | 51.227 |
| request-backpressure | 4 | W | 44,750.7 | 21.668 | 41.293 | 58.942 | 43.580 |
| request-backpressure | 4 | main | 42,559.5 | 22.806 | 49.097 | 67.973 | 51.477 |
| send-saturation | 1 | W | 175,035.1 | 4.699 | 19.473 | 19.909 | 23.364 |
| send-saturation | 1 | main | 175,715.8 | 5.428 | 25.362 | 25.729 | 23.876 |
| send-saturation | 2 | W | 178,922.7 | 4.684 | 19.450 | 19.873 | 23.409 |
| send-saturation | 2 | main | 175,532.8 | 5.407 | 25.332 | 25.682 | 23.888 |
| send-saturation | 3 | W | 177,605.7 | 4.673 | 19.456 | 19.889 | 23.452 |
| send-saturation | 3 | main | 174,614.4 | 5.419 | 25.450 | 25.797 | 23.788 |
| send-saturation | 4 | W | 171,628.4 | 4.808 | 19.443 | 19.882 | 23.569 |
| send-saturation | 4 | main | 172,555.4 | 5.452 | 25.264 | 25.620 | 23.794 |

| Pattern | Median build | msg/s | Submit CPU µs/msg | Submit heap calls/msg | Source heap/msg | Target heap/msg |
|---|---|---:|---:|---:|---:|---:|
| request-backpressure | W | 44,368.5 | 21.727 | 41.327 | 59.143 | 43.569 |
| request-backpressure | main | 42,629.0 | 22.495 | 49.341 | 67.961 | 51.604 |
| send-saturation | W | 176,320.4 | 4.691 | 19.453 | 19.885 | 23.430 |
| send-saturation | main | 175,073.6 | 5.424 | 25.347 | 25.705 | 23.835 |
