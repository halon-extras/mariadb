all: mariadb

mariadb:
	g++ -I/opt/halon/include/ -I/usr/include/mariadb/ -I/usr/include/mysql/ -I/usr/local/include/ -fPIC -shared mariadb.cpp -lmariadb -o mariadb.so
