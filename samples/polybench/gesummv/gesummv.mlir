func @gesummv(%alpha: f32, %beta: f32, %A: memref<32x32xf32>, %B: memref<32x32xf32>, %tmp: memref<32xf32>, %x: memref<32xf32>, %y: memref<32xf32>) {
  affine.for %i = 0 to 32 {
    %cst = constant 0.0 : f32
    affine.store %cst, %tmp[%i] : memref<32xf32>
    affine.store %cst, %y[%i] : memref<32xf32>
    affine.for %j = 0 to 32 {
      %0 = affine.load %A[%i, %j] : memref<32x32xf32>
      %1 = affine.load %x[%j] : memref<32xf32>
      %2 = affine.load %tmp[%i] : memref<32xf32>
      %3 = mulf %1, %0 : f32
      %4 = addf %3, %2 : f32
      affine.store %4, %tmp[%i] : memref<32xf32>
      %5 = affine.load %B[%i, %j] : memref<32x32xf32>
      %6 = affine.load %x[%j] : memref<32xf32>
      %7 = affine.load %y[%i] : memref<32xf32>
      %8 = mulf %6, %5 : f32
      %9 = addf %8, %7 : f32
      affine.store %9, %y[%i] : memref<32xf32>
    }
    %10 = affine.load %tmp[%i] : memref<32xf32>
    %11 = affine.load %y[%i] : memref<32xf32>
    %12 = mulf %alpha, %10 : f32
    %13 = mulf %beta, %11 : f32
    %14 = addf %13, %12 : f32
    affine.store %14, %y[%i] : memref<32xf32>
  }
  return
}
