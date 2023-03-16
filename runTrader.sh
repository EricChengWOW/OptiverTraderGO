 > autotrader.log
 > exchange.log
rm autotrader
cd build && make
cd ..
cp build/autotrader .
python3 rtg.py run autotrader